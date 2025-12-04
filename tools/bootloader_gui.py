#!/usr/bin/env python3
"""
Simple GUI uploader for the RP2040 keyboard bootloader (CDC).
Requirements:
  pip install pyserial
Tkinter is bundled with most Python distributions.
"""

import os
import threading
import time
import zlib
from pathlib import Path
from tkinter import Tk, StringVar, Text, END, DISABLED, NORMAL, filedialog, messagebox
from tkinter import ttk

import serial
import serial.tools.list_ports


BAUDRATE = 115200  # CDC ignores baud, but pyserial needs a value
WRITE_CHUNK = 256  # match flash page size to avoid overflow on device
READ_TIMEOUT = 1.0
CMD_TIMEOUT = 5.0
MAGIC_LINE = "MAGIC"


def list_serial_ports():
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append(p.device)
    return ports


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


class BootloaderClient:
    def __init__(self, port: str, log_cb):
        self.port = port
        self.log = log_cb
        self.ser = None

    def __enter__(self):
        self.ser = serial.Serial(self.port, BAUDRATE, timeout=READ_TIMEOUT)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.ser:
            self.ser.close()

    def _read_line(self, timeout=CMD_TIMEOUT):
        end_time = time.time() + timeout
        buf = bytearray()
        while time.time() < end_time:
            try:
                b = self.ser.read(1)
            except (serial.SerialException, OSError) as e:
                # Bubble up for caller to decide
                raise e
            if b:
                buf += b
                if b == b"\n":
                    break
            else:
                time.sleep(0.01)
        if not buf:
            return None
        line = buf.decode(errors="replace").strip()
        return line

    def _send_line(self, line: str):
        self.ser.write(line.encode("ascii"))
        self.ser.flush()

    def _expect_ok(self, timeout=CMD_TIMEOUT, allow_disconnect=False):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self._read_line(timeout=0.5)
            except (serial.SerialException, OSError) as e:
                if allow_disconnect:
                    return
                raise
            if line is None:
                continue
            if line.startswith("OK"):
                return
            if line.startswith("ERR"):
                raise RuntimeError(line)
            # Otherwise it's a log line; keep waiting for OK/ERR.
        raise RuntimeError("No OK response")

    def _drain_magic(self, timeout=5.0):
        end_time = time.time() + timeout
        while time.time() < end_time:
            line = self._read_line(timeout=0.5)
            if not line:
                continue
            if line == MAGIC_LINE:
                self.log("Magic packet received")
                return True
        return False

    def handshake(self):
        self._drain_magic(timeout=5.0)
        self._send_line("PING\n")
        deadline = time.time() + CMD_TIMEOUT
        while time.time() < deadline:
            resp = self._read_line(timeout=0.5)
            if resp is None:
                continue
            if resp == MAGIC_LINE:
                continue
            if resp == "PING":  # firmware echo; ignore and keep waiting
                continue
            if resp == "PONG":
                self.log("Connected (PONG)")
                return
            # Some devices reply OK to ping; accept that as well
            if resp.startswith("OK"):
                self.log("Connected (OK)")
                return
            raise RuntimeError(f"Unexpected response: {resp}")
        raise RuntimeError("Handshake timeout")

    def try_handshake(self, retries: int = 1, delay: float = 0.2) -> bool:
        for _ in range(retries):
            try:
                self.handshake()
                return True
            except Exception as e:
                self.log(f"Handshake failed: {e}")
                time.sleep(delay)
        return False

    def send_bootloader_reset(self):
        try:
            self._send_line("BOOTLOADER\n")
            self.ser.flush()
            # Try to read a quick response (e.g., "RESET") but don't block long.
            line = self._read_line(timeout=2.0)
            if line:
                self.log(f"{line}")
            else:
                self.log("No RESET response from firmware")
        except Exception as e:
            self.log(f"Failed to send BOOTLOADER reset: {e}")

    def wait_for_reenumeration(self, initial_port: str, timeout=3.0):
        end = time.time() + timeout
        last_seen = None
        last_log = 0
        while time.time() < end:
            ports = list_serial_ports()
            if initial_port in ports:
                last_seen = initial_port
                try:
                    with serial.Serial(initial_port, BAUDRATE, timeout=READ_TIMEOUT):
                        return initial_port
                except Exception as e:
                    now = time.time()
                    if now - last_log > 1.0:
                        self.log(f"Port busy: {e}")
                        last_log = now
            # try any new port that wasn't last seen
            for p in ports:
                if p != last_seen:
                    try:
                        with serial.Serial(p, BAUDRATE, timeout=READ_TIMEOUT):
                            return p
                    except Exception:
                        pass
            time.sleep(0.2)
        raise RuntimeError("Failed to re-enumerate bootloader")

    def connect_to_bootloader(self, initial_port: str, timeout=6.0) -> bool:
        """Close current port and retry the previous port (up to 3 times) for a bootloader handshake."""
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        time.sleep(0.5)

        for attempt in range(3):
            ports = list_serial_ports()
            if not (initial_port and initial_port in ports):
                time.sleep(0.2)
                continue
            try:
                ser = serial.Serial(initial_port, BAUDRATE, timeout=READ_TIMEOUT)
            except Exception as e:
                self.log(f"Port open failed {initial_port}: {e}")
                time.sleep(0.2)
                continue

            self.ser = ser
            self.port = initial_port
            if self.try_handshake(retries=1, delay=0.0):
                self.log(f"Reconnected to bootloader on {initial_port}")
                return True

            # handshake failed; close and retry
            try:
                ser.close()
            except Exception:
                pass
            self.ser = None
            time.sleep(0.2)

        return False

    def erase(self, length: int):
        self._send_line(f"ERASE 0x{length:X}\n")
        # Erasing can take time; wait generously.
        self._expect_ok(timeout=60.0)
        self.log(f"Erase OK (len=0x{length:X})")

    def write_chunk(self, offset: int, data: bytes):
        self._send_line(f"WRITE 0x{offset:X} 0x{len(data):X}\n")
        # raw payload immediately after the command
        self.ser.write(data)
        self.ser.flush()
        self._expect_ok()

    def finalize(self, total_len: int, crc_val: int):
        self._send_line(f"DONE 0x{total_len:X} 0x{crc_val:X}\n")
        # Device will reset after DONE; allow disconnect.
        self._expect_ok(timeout=15.0, allow_disconnect=True)
        self.log("Finalize OK")

    def boot(self):
        self._send_line("BOOT\n")
        # bootloader may jump away; ignore response

    def program(self, filepath: str, progress_cb):
        with open(filepath, "rb") as f:
            data = f.read()
        total_len = len(data)
        crc_val = crc32(data)
        self.log(f"Host CRC32: 0x{crc_val:08X} len={total_len}")

        if not self.try_handshake():
            # ask running firmware to reboot into bootloader
            self.log("Requesting reboot to bootloader...")
            self.send_bootloader_reset()
            if not self.connect_to_bootloader(self.port, timeout=6.0):
                raise RuntimeError("Failed to handshake after reboot")

        self.erase(total_len)

        offset = 0
        while offset < total_len:
            chunk = data[offset : offset + WRITE_CHUNK]
            self.write_chunk(offset, chunk)
            offset += len(chunk)
            progress_cb(offset, total_len)

        self.finalize(total_len, crc_val)
        self.log("Done.")


class App:
    def __init__(self, root):
        self.root = root
        self.root.title("RP2040 Keyboard Bootloader")
        self.port_var = StringVar()
        self.file_var = StringVar()
        self.log_widget = None
        self.ser = None

        self._build_ui()
        self.refresh_ports()

    def _build_ui(self):
        pad = {"padx": 6, "pady": 4}

        frm = ttk.Frame(self.root)
        frm.pack(fill="both", expand=True)

        row = 0
        ttk.Label(frm, text="Port").grid(row=row, column=0, sticky="w", **pad)
        self.port_combo = ttk.Combobox(frm, textvariable=self.port_var, width=25, state="readonly")
        self.port_combo.grid(row=row, column=1, **pad)
        ttk.Button(frm, text="Refresh", command=self.refresh_ports).grid(row=row, column=2, **pad)

        row += 1
        ttk.Label(frm, text="Binary").grid(row=row, column=0, sticky="w", **pad)
        ttk.Entry(frm, textvariable=self.file_var, width=40).grid(row=row, column=1, **pad)
        ttk.Button(frm, text="Browse", command=self.choose_file).grid(row=row, column=2, **pad)

        row += 1
        ttk.Button(frm, text="Flash", command=self.start_flash).grid(row=row, column=0, columnspan=3, sticky="we", **pad)

        row += 1
        self.log_widget = Text(frm, height=12, width=70, state=DISABLED)
        self.log_widget.grid(row=row, column=0, columnspan=3, sticky="nsew", **pad)
        frm.rowconfigure(row, weight=1)
        frm.columnconfigure(1, weight=1)

    def log(self, msg):
        self.log_widget.configure(state=NORMAL)
        self.log_widget.insert(END, msg + "\n")
        self.log_widget.see(END)
        self.log_widget.configure(state=DISABLED)

    def refresh_ports(self):
        ports = list_serial_ports()
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def choose_file(self):
        path = filedialog.askopenfilename(title="Select firmware binary", filetypes=[("Binary", "*.bin *.uf2 *.hex *.*")])
        if path:
            self.file_var.set(path)

    def set_widgets_state(self, enabled: bool):
        state = NORMAL if enabled else DISABLED
        for widget in (self.port_combo,):
            widget.configure(state="readonly" if enabled else DISABLED)

    def start_flash(self):
        port = self.port_var.get()
        filepath = self.file_var.get()

        if not port:
            self.log("Error: Select a port.")
            return
        if not filepath or not Path(filepath).is_file():
            self.log("Error: Select a valid binary file.")
            return

        self.set_widgets_state(False)
        self.log(f"Flashing {os.path.basename(filepath)} (staging->main) on {port}")

        thread = threading.Thread(
            target=self._flash_thread,
            args=(port, filepath),
            daemon=True,
        )
        thread.start()

    def _flash_thread(self, port, filepath):
        def progress(done, total):
            pct = done * 100 // total
            self.log(f"Write {done}/{total} ({pct}%)")

        try:
            with BootloaderClient(port, self.log) as client:
                client.program(filepath, progress)
        except Exception as e:
            self.log(f"Error: {e}")
        finally:
            # Clean up any dangling serial handle we may have left in connect_to_bootloader
            try:
                if self.ser:
                    self.ser.close()
            except Exception:
                pass
            self.ser = None
            self.set_widgets_state(True)


def main():
    root = Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
 

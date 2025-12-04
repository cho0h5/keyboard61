#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
#include "hardware/xip_cache.h"
#include "hardware/watchdog.h"
#include "hardware/resets.h"

// Flash map (offsets from flash base / XIP 0x10000000)
// Bootloader size is fixed (128 KB). The remaining flash is split in half at runtime:
//   Main slot (A) immediately after bootloader
//   Staging slot (B) after that
// Slot sizes are sector-aligned (4 KB).

#define FLASH_BASE_XIP          0x10000000u

#define BOOTLOADER_SIZE         0x00020000u
#define BOOT2_SIZE              0x00000100u

#define BOOT_TIMEOUT_MS         2000u
#define CDC_RX_TIMEOUT_MS       5000u
#define MAGIC_RETX_MS           500u

#define APP_MAGIC               0xB00710ADu
#define APP_VERSION             1u
#define BOOT_MAGIC_LINE         "MAGIC\n"

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t crc32;
    uint32_t version;
    uint32_t reserved;
} app_header_t;

typedef enum {
    SLOT_MAIN = 0,   // A
    SLOT_STAGING = 1 // B
} slot_select_t;

static bool update_mode = false;
static char line_buf[96];
static size_t line_len = 0;
static bool magic_sent = false;
static uint32_t last_magic_ms = 0;
static uint32_t flash_size_bytes = 0;
static uint32_t slot_size = 0;
static uint32_t slot_payload_size = 0;
static uint32_t slot_payload_offset = 0; // payload starts at slot base (no header in front)
static uint32_t slot_header_off = 0;     // header stored at end of slot
static uint32_t slot_main_off = 0;
static uint32_t slot_staging_off = 0;
static uint32_t crc_recv = 0;
static uint32_t crc_flash = 0;
static uint32_t bytes_received = 0;
static bool rx_in_progress = false;
static bool jump_pending = false;

static inline uint32_t crc32_init(void);
static uint32_t crc32_accum(uint32_t crc, const uint8_t *data, size_t len);
static inline uint32_t crc32_final(uint32_t crc);
static void reboot_board(void);
static inline void cdc_flush_and_delay(uint32_t ms);

static uint32_t align_up(uint32_t val, uint32_t align)
{
    return (val + align - 1u) & ~(align - 1u);
}

static void init_layout(void)
{
    // Use pico binary info if available; otherwise fall back to build-time define.
    // binary_info lookup is skipped here to keep bootloader minimal; assume 2MB if unset.
    #ifdef PICO_FLASH_SIZE_BYTES
    flash_size_bytes = PICO_FLASH_SIZE_BYTES;
    #else
    flash_size_bytes = 2u * 1024u * 1024u;
    #endif
    if (flash_size_bytes <= BOOTLOADER_SIZE + FLASH_SECTOR_SIZE * 2) {
        reboot_board(); // not enough space
    }

    uint32_t usable = flash_size_bytes - BOOTLOADER_SIZE;
    slot_size = align_up(usable / 2, FLASH_SECTOR_SIZE);
    if (slot_size < FLASH_SECTOR_SIZE * 8 || BOOTLOADER_SIZE + slot_size * 2 > flash_size_bytes) {
        reboot_board(); // slots too small
    }
    slot_payload_offset = 0;
    slot_header_off = slot_size - FLASH_PAGE_SIZE; // store header in last page of slot
    slot_payload_size = slot_header_off; // payload occupies start .. header-1
    slot_main_off = BOOTLOADER_SIZE;
    slot_staging_off = BOOTLOADER_SIZE + slot_size;
    crc_recv = crc32_init();
    crc_flash = crc32_init();
    bytes_received = 0;
    rx_in_progress = false;
}

static inline uint32_t slot_offset(slot_select_t slot)
{
    return slot == SLOT_MAIN ? slot_main_off : slot_staging_off;
}

static inline uint32_t slot_base_addr(slot_select_t slot)
{
    return FLASH_BASE_XIP + slot_offset(slot);
}

static void cdc_write_str(const char *s)
{
    tud_cdc_write_str(s);
    tud_cdc_write_flush();
}

static inline uint32_t crc32_init(void) { return 0xFFFFFFFFu; }

static uint32_t crc32_accum(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static inline uint32_t crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

static void reboot_board(void)
{
    if (tud_cdc_connected()) {
        tud_cdc_write_str("REBOOT\n");
        tud_cdc_write_flush();
        sleep_ms(30);
    }
    watchdog_reboot(0, 0, 0);
    while (1) { }
}

static inline void cdc_flush_and_delay(uint32_t ms)
{
    tud_cdc_write_flush();
    if (ms) sleep_ms(ms);
}

static uint32_t crc32_flash_region(uint32_t xip_addr, uint32_t length)
{
    const uint8_t *ptr = (const uint8_t *)xip_addr;
    uint32_t crc = crc32_init();
    while (length) {
        uint32_t chunk = length > 1024 ? 1024 : length;
        crc = crc32_accum(crc, ptr, chunk);
        ptr += chunk;
        length -= chunk;
    }
    return crc32_final(crc);
}

static bool validate_slot(slot_select_t slot)
{
    const app_header_t *hdr = (const app_header_t *)(slot_base_addr(slot) + slot_header_off);
    if (hdr->magic != APP_MAGIC) {
        return false;
    }
    if (hdr->length == 0 || hdr->length > slot_payload_size) {
        return false;
    }
    uint32_t payload_addr = slot_base_addr(slot) + slot_payload_offset;
    uint32_t crc = crc32_flash_region(payload_addr, hdr->length);
    return crc == hdr->crc32;
}

static void jump_to_slot(slot_select_t slot)
{
    uint32_t image_base = slot_base_addr(slot) + slot_payload_offset;
    uint32_t app_addr = image_base + BOOT2_SIZE; // skip boot2, jump to vector table
    uint32_t sp = *(uint32_t *)(app_addr + 0);
    uint32_t pc = *(uint32_t *)(app_addr + 4);

    if (sp < 0x20000000u || sp > 0x20042000u || pc < FLASH_BASE_XIP || pc > (FLASH_BASE_XIP + flash_size_bytes)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "JUMP FAIL sp=0x%08lX pc=0x%08lX\n", (unsigned long)sp, (unsigned long)pc);
        cdc_write_str(msg);
        cdc_flush_and_delay(10);
        reboot_board();
    }

    char msg[64];
    const char *name = slot == SLOT_MAIN ? "MAIN" : "STAGING";
    snprintf(msg, sizeof(msg), "JUMP %s sp=0x%08lX pc=0x%08lX\n", name, (unsigned long)sp, (unsigned long)pc);
    cdc_write_str(msg);
    cdc_flush_and_delay(5); // give host a chance to read the log

    // Force USB block to reset so the application re-enumerates cleanly.
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);

    __asm volatile ("cpsid i");
    __asm volatile ("msr msp, %0" :: "r" (sp) : );
    scb_hw->vtor = app_addr;
    ((void (*)(void))pc)();
}

static bool parse_hex_u32(const char *s, uint32_t *out)
{
    if (!s) return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

static bool read_exact(uint8_t *buf, uint32_t len)
{
    uint32_t got = 0;
    absolute_time_t deadline = make_timeout_time_ms(CDC_RX_TIMEOUT_MS);
    while (got < len) {
        tud_task(); // keep USB serviced
        uint32_t n = tud_cdc_read(buf + got, len - got);
        if (n) {
            got += n;
            continue;
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            return false;
        }
        sleep_ms(1);
    }
    return true;
}

static bool __not_in_flash_func(erase_slot)(slot_select_t slot)
{
    uint32_t off = slot_offset(slot);
    if (off + slot_size > flash_size_bytes) return false;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, slot_size);
    restore_interrupts(ints);
    return true;
}

static bool __not_in_flash_func(erase_staging_len)(uint32_t len)
{
    if (len == 0) return false;
    uint32_t padded = align_up(len, FLASH_SECTOR_SIZE);
    uint32_t must_cover = align_up(slot_header_off + FLASH_PAGE_SIZE, FLASH_SECTOR_SIZE); // ensure header sector is erased
    if (padded < must_cover) padded = must_cover;
    if (padded > slot_size) padded = slot_size;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(slot_staging_off, padded);
    restore_interrupts(ints);
    crc_recv = crc32_init();
    crc_flash = crc32_init();
    bytes_received = 0;
    rx_in_progress = true;
    return true;
}

static bool __not_in_flash_func(program_block)(uint32_t flash_off, const uint8_t *src, size_t len)
{
    // len must be multiple of FLASH_PAGE_SIZE
    if ((len % FLASH_PAGE_SIZE) != 0) return false;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(flash_off, src, len);
    restore_interrupts(ints);
    return true;
}

static bool __not_in_flash_func(write_header)(slot_select_t slot, uint32_t total_len, uint32_t expected_crc)
{
    if (total_len == 0 || total_len > slot_payload_size) return false;

    uint8_t header_page[FLASH_PAGE_SIZE];
    memset(header_page, 0xFF, sizeof(header_page));
    app_header_t hdr = {
        .magic = APP_MAGIC,
        .length = total_len,
        .crc32 = expected_crc,
        .version = APP_VERSION,
        .reserved = 0,
    };
    memcpy(header_page, &hdr, sizeof(hdr));

    return program_block(slot_offset(slot) + slot_header_off, header_page, sizeof(header_page));
}

static bool __not_in_flash_func(handle_write)(slot_select_t slot, uint32_t data_offset, uint32_t data_len)
{
    if (data_len == 0) {
        cdc_write_str("ERR empty\n");
        return false;
    }

    uint32_t padded = (data_len + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
    if (data_offset + padded > slot_payload_size) {
        cdc_write_str("ERR range\n");
        return false;
    }

    if (rx_in_progress && data_offset != bytes_received) {
        cdc_write_str("ERR offset\n");
        return false;
    }

    uint32_t flash_off = slot_offset(slot) + slot_payload_offset + data_offset;
    uint32_t remaining = data_len;
    uint8_t page[FLASH_PAGE_SIZE];

    while (remaining) {
        uint32_t to_read = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));
        if (!read_exact(page, to_read)) {
            cdc_write_str("ERR timeout\n");
            return false;
        }
        uint32_t dest = flash_off;
        if (!program_block(dest, page, FLASH_PAGE_SIZE)) {
            cdc_write_str("ERR prog\n");
            return false;
        }

        flash_flush_cache();
        xip_cache_invalidate_range(dest, FLASH_PAGE_SIZE);

        // Verify the chunk we just wrote (only the meaningful bytes).
        const uint8_t *flash_ptr = (const uint8_t *)(FLASH_BASE_XIP + dest);
        if (memcmp(flash_ptr, page, to_read) != 0) {
            // Find first mismatch for diagnostics
            uint32_t mismatch = 0;
            for (; mismatch < to_read; mismatch++) {
                if (flash_ptr[mismatch] != page[mismatch]) break;
            }
            char msg[80];
            snprintf(msg, sizeof(msg), "ERR verify off=0x%lX exp=0x%02X got=0x%02X\n",
                     (unsigned long)(dest + mismatch),
                     (unsigned int)page[mismatch],
                     (unsigned int)flash_ptr[mismatch]);
            cdc_write_str(msg);
            return false;
        }

        crc_recv = crc32_accum(crc_recv, page, to_read);
        crc_flash = crc32_accum(crc_flash, flash_ptr, to_read);
        bytes_received += to_read;
        flash_off += FLASH_PAGE_SIZE;
        remaining -= to_read;
    }

    flash_flush_cache(); // ensure subsequent reads see fresh data
    cdc_write_str("OK\n");
    return true;
}

static bool __not_in_flash_func(copy_staging_to_main)(uint32_t total_len, uint32_t expected_crc)
{
    if (total_len == 0 || total_len > slot_payload_size) {
        cdc_write_str("ERR len\n");
        return false;
    }

    uint32_t staging_payload = slot_base_addr(SLOT_STAGING) + slot_payload_offset;
    flash_flush_cache();
    xip_cache_invalidate_all();
    uint32_t crc = crc32_flash_region(staging_payload, total_len);
    uint32_t crc_rx = expected_crc;
    uint32_t crc_wr = expected_crc;
    uint32_t rx_len = total_len;
    if (rx_in_progress) {
        crc_rx = crc32_final(crc_recv);
        crc_wr = crc32_final(crc_flash);
        rx_len = bytes_received;
    }
    rx_in_progress = false;

    if (crc != expected_crc || crc_rx != expected_crc || crc_wr != expected_crc || rx_len != total_len) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "ERR crc calc=0x%08lX rx=0x%08lX wr=0x%08lX exp=0x%08lX len=0x%lX rxlen=0x%lX\n",
                 (unsigned long)crc, (unsigned long)crc_rx, (unsigned long)crc_wr, (unsigned long)expected_crc,
                 (unsigned long)total_len, (unsigned long)rx_len);
        cdc_write_str(msg);
        return false;
    }

    // Mark staging as valid so a subsequent boot can recover even if host disconnects.
    if (!write_header(SLOT_STAGING, total_len, expected_crc)) {
        cdc_write_str("ERR header_stg\n");
        return false;
    }

    // Erase main slot
    if (!erase_slot(SLOT_MAIN)) {
        cdc_write_str("ERR erase_main\n");
        return false;
    }

    // Copy payload from staging to main
    uint32_t remaining = total_len;
    uint32_t src = staging_payload;
    uint32_t dst_off = slot_offset(SLOT_MAIN) + slot_payload_offset;
    uint8_t page[FLASH_PAGE_SIZE];
    while (remaining) {
        uint32_t chunk = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));
        memcpy(page, (const void *)src, chunk);
        if (!program_block(dst_off, page, FLASH_PAGE_SIZE)) {
            cdc_write_str("ERR prog_main\n");
            return false;
        }
        src += chunk;
        dst_off += FLASH_PAGE_SIZE;
        remaining -= chunk;
    }

    // Write header to main
    if (!write_header(SLOT_MAIN, total_len, expected_crc)) {
        cdc_write_str("ERR header_main\n");
        return false;
    }

    flash_flush_cache();
    xip_cache_invalidate_all();

    // Verify main slot CRC matches
    uint32_t main_payload = slot_base_addr(SLOT_MAIN) + slot_payload_offset;
    uint32_t main_crc = crc32_flash_region(main_payload, total_len);
    if (main_crc != expected_crc) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "ERR crc_main calc=0x%08lX exp=0x%08lX\n",
                 (unsigned long)main_crc, (unsigned long)expected_crc);
        cdc_write_str(msg);
        return false;
    }

    // Erase staging after successful copy (best effort)
    cdc_write_str("OK\n");
    return true;
}

static void handle_line(char *line)
{
    update_mode = true;
    // Trim trailing CR/LF
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
        line[--n] = '\0';
    }
    if (n == 0) return;

    char *save;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) return;

    if (strcmp(cmd, "PING") == 0) {
        cdc_write_str("PONG\n");
        return;
    }

    if (strcmp(cmd, "INFO") == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "BOOT BL=0x%05X MAIN=0x%05X STG=0x%05X SZ=0x%05X\n",
                 BOOTLOADER_SIZE, slot_offset(SLOT_MAIN), slot_offset(SLOT_STAGING), slot_size);
        cdc_write_str(msg);
        return;
    }

    if (strcmp(cmd, "ERASE") == 0) {
        char *len_s = strtok_r(NULL, " \t", &save);
        uint32_t len = slot_size;
        if (len_s) {
            if (!parse_hex_u32(len_s, &len)) {
                cdc_write_str("ERR args\n");
                return;
            }
        }
        if (erase_staging_len(len)) {
            cdc_write_str("OK\n");
        } else {
            cdc_write_str("ERR erase\n");
        }
        return;
    }

    if (strcmp(cmd, "WRITE") == 0) {
        char *off_s = strtok_r(NULL, " \t", &save);
        char *len_s = strtok_r(NULL, " \t", &save);
        uint32_t off = 0, len = 0;
        if (!parse_hex_u32(off_s, &off) || !parse_hex_u32(len_s, &len)) {
            cdc_write_str("ERR args\n");
            return;
        }
        if (!handle_write(SLOT_STAGING, off, len)) {
            cdc_flush_and_delay(10);
            reboot_board();
        }
        return;
    }

    if (strcmp(cmd, "DONE") == 0) {
        char *len_s = strtok_r(NULL, " \t", &save);
        char *crc_s = strtok_r(NULL, " \t", &save);
        char *opt_s = strtok_r(NULL, " \t", &save);
        uint32_t len = 0, crc = 0;
        if (!parse_hex_u32(len_s, &len) || !parse_hex_u32(crc_s, &crc)) {
            cdc_write_str("ERR args\n");
            return;
        }
        if (copy_staging_to_main(len, crc)) {
            cdc_write_str("DONE OK\n");
            cdc_flush_and_delay(30);
            reboot_board();
        }
        return;
    }

    if (strcmp(cmd, "RESET") == 0) {
        cdc_write_str("OK\n");
        cdc_flush_and_delay(10);
        reboot_board();
        return;
    }

    cdc_write_str("ERR cmd\n");
}

static void pump_cdc(void)
{
    if (!tud_cdc_available()) return;

    uint8_t buf[64];
    uint32_t count = tud_cdc_read(buf, sizeof(buf));
    for (uint32_t i = 0; i < count; i++) {
        char c = (char)buf[i];
        if (line_len + 1 >= sizeof(line_buf)) {
            line_len = 0;
            continue;
        }
        line_buf[line_len++] = c;
        if (c == '\n') {
            line_buf[line_len] = '\0';
            handle_line(line_buf);
            line_len = 0;
        }
    }
}

int main(void)
{
    board_init();
    tusb_init();
    init_layout();

    uint32_t start_ms = board_millis();

    while (1) {
        tud_task();
        pump_cdc();

        bool connected = tud_cdc_connected();
        if (connected && !update_mode) {
            uint32_t now = board_millis();
            if (!magic_sent || (now - last_magic_ms) >= MAGIC_RETX_MS) {
                tud_cdc_write_str(BOOT_MAGIC_LINE);
                tud_cdc_write_flush();
                magic_sent = true;
                last_magic_ms = now;
            }
        }

        if (!update_mode && (board_millis() - start_ms) > BOOT_TIMEOUT_MS) {
            if (validate_slot(SLOT_STAGING)) {
                const app_header_t *hdr = (const app_header_t *)(slot_base_addr(SLOT_STAGING) + slot_header_off);
                if (copy_staging_to_main(hdr->length, hdr->crc32)) {
                    erase_slot(SLOT_STAGING);
                }
                reboot_board();
            } else if (validate_slot(SLOT_MAIN)) {
                jump_to_slot(SLOT_MAIN);
            } else {
                reboot_board();
            }
        }
    }
}

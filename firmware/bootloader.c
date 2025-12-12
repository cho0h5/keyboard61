#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/scb.h"
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
#define CDC_RX_TIMEOUT_MS       200u
#define APP_MAGIC               0xB00710ADu
#define APP_VERSION             1u
#define MAX_IMAGE_SIZE          (128 * 1024u) // limit buffered image size to fit in RAM

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
static uint32_t last_progress_ms = 0;
static uint8_t staging_buf[MAX_IMAGE_SIZE];

static inline uint32_t crc32_init(void);
static uint32_t crc32_accum(uint32_t crc, const uint8_t *data, size_t len);
static inline uint32_t crc32_final(uint32_t crc);
static void reboot_board(void);
static inline void cdc_flush_and_delay(uint32_t ms);
static void reset_rx_state(void);

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

// Clear RX state and drain any buffered USB data.
static void reset_rx_state(void)
{
    line_len = 0;
    bytes_received = 0;
    rx_in_progress = false;
    crc_recv = crc32_init();
    crc_flash = crc32_init();
    uint8_t tmp[64];
    while (tud_cdc_available()) {
        tud_cdc_read(tmp, sizeof(tmp));
    }
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

// Read a line (terminated by '\n') into buf (null-terminated). Returns false on timeout.
static bool read_line_blocking(char *buf, size_t buf_sz)
{
    if (buf_sz == 0) return false;
    size_t pos = 0;
    absolute_time_t deadline = make_timeout_time_ms(CDC_RX_TIMEOUT_MS);
    while (1) {
        tud_task_ext(0, false);
        uint8_t ch;
        uint32_t n = tud_cdc_read(&ch, 1);
        if (n) {
            if (pos + 1 < buf_sz) {
                buf[pos++] = (char)ch;
            }
            if (ch == '\n') {
                buf[pos] = '\0';
                return true;
            }
            deadline = make_timeout_time_ms(CDC_RX_TIMEOUT_MS); // progress resets timeout
            continue;
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            return false;
        }
        sleep_ms(1);
    }
}

static int hex_char(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Read len bytes encoded as ASCII hex (2 chars per byte) followed by '\n'.
static bool read_hex_payload(uint8_t *out, uint32_t len)
{
    // max characters: 2*len + newline
    char line[1024];
    if (len * 2 + 2 > sizeof(line)) return false;
    if (!read_line_blocking(line, sizeof(line))) {
        return false;
    }

    size_t line_len = strlen(line);
    // strip trailing CR/LF
    while (line_len && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
        line[--line_len] = '\0';
    }
    if (line_len != len * 2) return false;

    for (uint32_t i = 0; i < len; i++) {
        int hi = hex_char(line[i * 2]);
        int lo = hex_char(line[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
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
    rx_in_progress = false;
    last_progress_ms = board_millis();
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

    if (data_offset + data_len > MAX_IMAGE_SIZE) {
        cdc_write_str("ERR toolong\n");
        return false;
    }

    if (rx_in_progress && data_offset != bytes_received) {
        cdc_write_str("ERR offset\n");
        return false;
    }

    if (!rx_in_progress) {
        crc_recv = crc32_init();
        crc_flash = crc32_init();
        bytes_received = 0;
        rx_in_progress = true;
    }
    last_progress_ms = board_millis();

    uint32_t flash_off = slot_offset(slot) + slot_payload_offset + data_offset;
    uint32_t remaining = data_len;
    uint8_t page[FLASH_PAGE_SIZE];

    while (remaining) {
        uint32_t to_read = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));
        if (!read_hex_payload(page, to_read)) {
            cdc_write_str("ERR timeout\n");
            reset_rx_state();
            return false;
        }
        memcpy(staging_buf + data_offset + (data_len - remaining), page, to_read);

        crc_recv = crc32_accum(crc_recv, page, to_read);
        bytes_received += to_read;
        last_progress_ms = board_millis();
        flash_off += FLASH_PAGE_SIZE;
        remaining -= to_read;
    }

        cdc_write_str("OK\n");
        last_progress_ms = board_millis();
        return true;
}

static bool __not_in_flash_func(copy_staging_to_main)(uint32_t total_len, uint32_t expected_crc)
{
    // This is used on boot to copy staging (flash) to main.
    if (total_len == 0 || total_len > slot_payload_size) {
        return false;
    }

    uint32_t staging_payload = slot_base_addr(SLOT_STAGING) + slot_payload_offset;
    flash_flush_cache();
    uint32_t crc = crc32_flash_region(staging_payload, total_len);
    if (crc != expected_crc) {
        return false;
    }

    if (!erase_slot(SLOT_MAIN)) {
        return false;
    }

    uint32_t remaining = total_len;
    uint32_t src = staging_payload;
    uint32_t dst_off = slot_offset(SLOT_MAIN) + slot_payload_offset;
    uint8_t page[FLASH_PAGE_SIZE];
    while (remaining) {
        uint32_t chunk = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));
        memcpy(page, (const void *)src, chunk);
        if (!program_block(dst_off, page, FLASH_PAGE_SIZE)) {
            return false;
        }
        src += chunk;
        dst_off += FLASH_PAGE_SIZE;
        remaining -= chunk;
    }

    if (!write_header(SLOT_MAIN, total_len, expected_crc)) {
        return false;
    }

    flash_flush_cache();
    uint32_t main_payload = slot_base_addr(SLOT_MAIN) + slot_payload_offset;
    uint32_t main_crc = crc32_flash_region(main_payload, total_len);
    if (main_crc != expected_crc) {
        return false;
    }

    erase_slot(SLOT_STAGING);
    return true;
}

// Program RAM buffer into staging and leave copy to main for next boot cycle.
static bool program_staging_from_buffer(uint32_t total_len, uint32_t expected_crc)
{
    if (total_len == 0 || total_len > slot_payload_size || total_len > MAX_IMAGE_SIZE) {
        cdc_write_str("ERR len\n");
        return false;
    }
    if (bytes_received != total_len) {
        cdc_write_str("ERR len_mismatch\n");
        return false;
    }

    uint32_t crc_buf = crc32_final(crc_recv);
    if (crc_buf != expected_crc) {
        char msg[80];
        snprintf(msg, sizeof(msg), "ERR crc buf=0x%08lX exp=0x%08lX\n",
                 (unsigned long)crc_buf, (unsigned long)expected_crc);
        cdc_write_str(msg);
        return false;
    }

    if (!erase_staging_len(total_len)) {
        cdc_write_str("ERR erase\n");
        return false;
    }

    uint32_t dst_off = slot_offset(SLOT_STAGING) + slot_payload_offset;
    uint32_t remaining = total_len;
    uint32_t buf_off = 0;
    uint8_t page[FLASH_PAGE_SIZE];
    while (remaining) {
        uint32_t chunk = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));
        memcpy(page, staging_buf + buf_off, chunk);
        if (!program_block(dst_off, page, FLASH_PAGE_SIZE)) {
            cdc_write_str("ERR prog_stg\n");
            return false;
        }
        dst_off += FLASH_PAGE_SIZE;
        buf_off += chunk;
        remaining -= chunk;
    }

    if (!write_header(SLOT_STAGING, total_len, expected_crc)) {
        cdc_write_str("ERR header_stg\n");
        return false;
    }

    flash_flush_cache();
    uint32_t staging_payload = slot_base_addr(SLOT_STAGING) + slot_payload_offset;
    uint32_t crc_stg = crc32_flash_region(staging_payload, total_len);
    if (crc_stg != expected_crc) {
        char msg[80];
        snprintf(msg, sizeof(msg), "ERR crc_stg calc=0x%08lX exp=0x%08lX\n",
                 (unsigned long)crc_stg, (unsigned long)expected_crc);
        cdc_write_str(msg);
        return false;
    }

    reset_rx_state();
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
            // allow host to retry the same chunk
            return;
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
        if (program_staging_from_buffer(len, crc)) {
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
    while (tud_cdc_available()) {
        char c;
        uint32_t n = tud_cdc_read(&c, 1);
        if (n == 0) {
            break;
        }

        if (line_len + 1 >= sizeof(line_buf)) {
            line_len = 0;
            continue;
        }

        line_buf[line_len++] = c;
        last_progress_ms = board_millis();

        if (c == '\n') {
            line_buf[line_len] = '\0';
            handle_line(line_buf);
            line_len = 0;
            return;
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
        tud_task_ext(0, false);
        pump_cdc();

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

        // If an update was started but no progress for >2s, reboot to recover.
        if (rx_in_progress && (board_millis() - last_progress_ms) > 2000u) {
            cdc_write_str("ERR timeout\n");
            cdc_flush_and_delay(10);
            reboot_board();
        }
    }
}

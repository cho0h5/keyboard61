#include <stdio.h>
#include "pico/stdlib.h"

#include "bsp/board_api.h"
#include "tusb.h"

#define HID_KEY_Z 0x1D

void send_key(uint8_t keycode)
{
    if (!tud_hid_ready()) return;

    uint8_t report[8] = {0};
    report[2] = keycode;
    tud_hid_keyboard_report(1, 0, report);

    sleep_ms(10);

    uint8_t empty_report[8] = {0};
    tud_hid_keyboard_report(1, 0, empty_report);
}

int main()
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);

    uint32_t last_press = 0;

    while (true) {
        tud_task();

        uint32_t now = board_millis();
        if (now - last_press >= 2000) {
            send_key(HID_KEY_Z);
            last_press = now;
        }
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) reqlen;

  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
  (void) instance;
  (void) report_id;
  (void) report_type;
  (void) buffer;
  (void) bufsize;
}
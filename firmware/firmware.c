#include <stdio.h>
#include "pico/stdlib.h"

#include "bsp/board_api.h"
#include "tusb.h"

#define HID_KEY_Z 0x1D
#define LED_PIN PICO_DEFAULT_LED_PIN
#define SWITCH_PIN 15

int main()
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_init(SWITCH_PIN);
    gpio_set_dir(SWITCH_PIN, GPIO_IN);
    gpio_pull_up(SWITCH_PIN);

    bool last_switch_state = false;
    uint32_t last_change_time = 0;

    while (true) {
        tud_task();

        // Read switch state (inverted because of pull-up)
        bool current_switch_state = !gpio_get(SWITCH_PIN);
        uint32_t now = board_millis();

        // Debouncing: check if state changed and enough time has passed
        if (current_switch_state != last_switch_state && (now - last_change_time) >= 20) {
            if (tud_hid_ready()) {
                uint8_t keycodes[6] = {current_switch_state ? HID_KEY_Z : 0, 0, 0, 0, 0, 0};
                tud_hid_keyboard_report(0, 0, keycodes);
                gpio_put(LED_PIN, current_switch_state);
            }
            last_switch_state = current_switch_state;
            last_change_time = now;
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
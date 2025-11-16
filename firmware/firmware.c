#include <stdio.h>
#include "pico/stdlib.h"

#include "bsp/board_api.h"
#include "tusb.h"

// HID Keycodes
#define HID_KEY_A 0x04
#define HID_KEY_B 0x05
#define HID_KEY_C 0x06
#define HID_KEY_D 0x07

// Pin definitions
#define LED_PIN PICO_DEFAULT_LED_PIN
#define ROW0_PIN 14
#define ROW1_PIN 15
#define COL0_PIN 16
#define COL1_PIN 17

// Matrix configuration
#define NUM_ROWS 2
#define NUM_COLS 2

// Keymap: [row][col]
const uint8_t keymap[NUM_ROWS][NUM_COLS] = {
    {HID_KEY_A, HID_KEY_B},  // Row 0
    {HID_KEY_C, HID_KEY_D}   // Row 1
};

// Pin arrays
const uint8_t row_pins[NUM_ROWS] = {ROW0_PIN, ROW1_PIN};
const uint8_t col_pins[NUM_COLS] = {COL0_PIN, COL1_PIN};

// Scan matrix and return pressed keycode (0 if no key pressed)
uint8_t scan_matrix(void)
{
    for (int row = 0; row < NUM_ROWS; row++) {
        // Set current row LOW
        gpio_put(row_pins[row], 0);
        sleep_us(1);  // Small delay for signal to stabilize

        // Read all columns
        for (int col = 0; col < NUM_COLS; col++) {
            bool is_pressed = !gpio_get(col_pins[col]);  // LOW = pressed

            if (is_pressed) {
                // Set current row back to HIGH before returning
                gpio_put(row_pins[row], 1);
                return keymap[row][col];
            }
        }

        // Set current row back to HIGH
        gpio_put(row_pins[row], 1);
    }

    return 0;  // No key pressed
}

int main()
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);

    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // Initialize row pins as outputs (will be set HIGH by default, then LOW when scanning)
    for (int i = 0; i < NUM_ROWS; i++) {
        gpio_init(row_pins[i]);
        gpio_set_dir(row_pins[i], GPIO_OUT);
        gpio_put(row_pins[i], 1);  // HIGH by default
    }

    // Initialize column pins as inputs with pull-up
    for (int i = 0; i < NUM_COLS; i++) {
        gpio_init(col_pins[i]);
        gpio_set_dir(col_pins[i], GPIO_IN);
        gpio_pull_up(col_pins[i]);
    }

    uint32_t last_change_time = 0;

    while (true) {
        tud_task();

        uint32_t now = board_millis();
        uint8_t pressed_keycode = scan_matrix();

        // Debouncing and sending HID report
        if ((now - last_change_time) >= 20) {
            if (tud_hid_ready()) {
                uint8_t keycodes[6] = {0};
                if (pressed_keycode != 0) {
                    keycodes[0] = pressed_keycode;
                }
                tud_hid_keyboard_report(0, 0, keycodes);
                gpio_put(LED_PIN, pressed_keycode != 0);
            }
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
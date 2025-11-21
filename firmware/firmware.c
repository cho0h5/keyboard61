#include <stdio.h>
#include "pico/stdlib.h"

#include "bsp/board_api.h"
#include "tusb.h"

// HID Keycodes - Standard US Keyboard Layout
#define HID_KEY_NONE        0x00
#define HID_KEY_A           0x04
#define HID_KEY_B           0x05
#define HID_KEY_C           0x06
#define HID_KEY_D           0x07
#define HID_KEY_E           0x08
#define HID_KEY_F           0x09
#define HID_KEY_G           0x0A
#define HID_KEY_H           0x0B
#define HID_KEY_I           0x0C
#define HID_KEY_J           0x0D
#define HID_KEY_K           0x0E
#define HID_KEY_L           0x0F
#define HID_KEY_M           0x10
#define HID_KEY_N           0x11
#define HID_KEY_O           0x12
#define HID_KEY_P           0x13
#define HID_KEY_Q           0x14
#define HID_KEY_R           0x15
#define HID_KEY_S           0x16
#define HID_KEY_T           0x17
#define HID_KEY_U           0x18
#define HID_KEY_V           0x19
#define HID_KEY_W           0x1A
#define HID_KEY_X           0x1B
#define HID_KEY_Y           0x1C
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_2           0x1F
#define HID_KEY_3           0x20
#define HID_KEY_4           0x21
#define HID_KEY_5           0x22
#define HID_KEY_6           0x23
#define HID_KEY_7           0x24
#define HID_KEY_8           0x25
#define HID_KEY_9           0x26
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESCAPE      0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_MINUS       0x2D
#define HID_KEY_EQUAL       0x2E
#define HID_KEY_BRACKET_LEFT  0x2F
#define HID_KEY_BRACKET_RIGHT 0x30
#define HID_KEY_BACKSLASH   0x31
#define HID_KEY_SEMICOLON   0x33
#define HID_KEY_APOSTROPHE  0x34
#define HID_KEY_GRAVE       0x35
#define HID_KEY_COMMA       0x36
#define HID_KEY_PERIOD      0x37
#define HID_KEY_SLASH       0x38
#define HID_KEY_CAPS_LOCK   0x39
#define HID_KEY_F1          0x3A
#define HID_KEY_F2          0x3B
#define HID_KEY_F3          0x3C
#define HID_KEY_F4          0x3D
#define HID_KEY_F5          0x3E
#define HID_KEY_F6          0x3F
#define HID_KEY_F7          0x40
#define HID_KEY_F8          0x41
#define HID_KEY_F9          0x42
#define HID_KEY_F10         0x43
#define HID_KEY_F11         0x44
#define HID_KEY_F12         0x45
#define HID_KEY_PRINT_SCREEN 0x46
#define HID_KEY_SCROLL_LOCK 0x47
#define HID_KEY_PAUSE       0x48
#define HID_KEY_INSERT      0x49
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGE_UP     0x4B
#define HID_KEY_DELETE      0x4C
#define HID_KEY_END         0x4D
#define HID_KEY_PAGE_DOWN   0x4E
#define HID_KEY_ARROW_RIGHT 0x4F
#define HID_KEY_ARROW_LEFT  0x50
#define HID_KEY_ARROW_DOWN  0x51
#define HID_KEY_ARROW_UP    0x52
#define HID_KEY_CONTROL_LEFT  0xE0
#define HID_KEY_SHIFT_LEFT    0xE1
#define HID_KEY_ALT_LEFT      0xE2
#define HID_KEY_GUI_LEFT      0xE3
#define HID_KEY_CONTROL_RIGHT 0xE4
#define HID_KEY_SHIFT_RIGHT   0xE5
#define HID_KEY_ALT_RIGHT     0xE6
#define HID_KEY_GUI_RIGHT     0xE7
#define HID_KEY_FN            0xF0  // Custom FN key

// Pin definitions
#define LED_PIN PICO_DEFAULT_LED_PIN

// Matrix configuration - 5 rows x 14 columns (61 keys)
#define NUM_ROWS 5
#define NUM_COLS 14

// Row pins: GPIO0-4 (connected to R1-R5 via pins 2-6)
#define ROW1_PIN 0  // GPIO0 -> Pin 2 -> R1
#define ROW2_PIN 1  // GPIO1 -> Pin 3 -> R2
#define ROW3_PIN 2  // GPIO2 -> Pin 4 -> R3
#define ROW4_PIN 3  // GPIO3 -> Pin 5 -> R4
#define ROW5_PIN 4  // GPIO4 -> Pin 6 -> R5

// Column pins: GPIO5-18 (connected to C1-C14)
#define COL1_PIN  5   // GPIO5  -> Pin 7  -> C1
#define COL2_PIN  6   // GPIO6  -> Pin 8  -> C2
#define COL3_PIN  7   // GPIO7  -> Pin 9  -> C3
#define COL4_PIN  8   // GPIO8  -> Pin 11 -> C4
#define COL5_PIN  9   // GPIO9  -> Pin 12 -> C5
#define COL6_PIN  10  // GPIO10 -> Pin 13 -> C6
#define COL7_PIN  11  // GPIO11 -> Pin 14 -> C7
#define COL8_PIN  12  // GPIO12 -> Pin 15 -> C8
#define COL9_PIN  13  // GPIO13 -> Pin 16 -> C9
#define COL10_PIN 14  // GPIO14 -> Pin 17 -> C10
#define COL11_PIN 15  // GPIO15 -> Pin 18 -> C11
#define COL12_PIN 16  // GPIO16 -> Pin 27 -> C12
#define COL13_PIN 17  // GPIO17 -> Pin 28 -> C13
#define COL14_PIN 18  // GPIO18 -> Pin 29 -> C14

// Pin arrays
const uint8_t row_pins[NUM_ROWS] = {ROW1_PIN, ROW2_PIN, ROW3_PIN, ROW4_PIN, ROW5_PIN};
const uint8_t col_pins[NUM_COLS] = {COL1_PIN, COL2_PIN, COL3_PIN, COL4_PIN, COL5_PIN, COL6_PIN, COL7_PIN, COL8_PIN, COL9_PIN, COL10_PIN, COL11_PIN, COL12_PIN, COL13_PIN, COL14_PIN};

// Keymap: [row][col] - Standard 60% keyboard layout
// Columns: C1(GPIO5), C2(GPIO6), C3(GPIO7), C4(GPIO8), C5(GPIO9), C6(GPIO10), C7(GPIO11), C8(GPIO12), C9(GPIO13), C10(GPIO14), C11(GPIO15), C12(GPIO16), C13(GPIO17), C14(GPIO18)
const uint8_t keymap[NUM_ROWS][NUM_COLS] = {
    // Row 1 (R1 - GPIO0): Number row
    {HID_KEY_GRAVE, HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5, HID_KEY_6, HID_KEY_7, HID_KEY_8, HID_KEY_9, HID_KEY_0, HID_KEY_MINUS, HID_KEY_EQUAL, HID_KEY_BACKSPACE},

    // Row 2 (R2 - GPIO1): Top letter row
    {HID_KEY_TAB, HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R, HID_KEY_T, HID_KEY_Y, HID_KEY_U, HID_KEY_I, HID_KEY_O, HID_KEY_P, HID_KEY_BRACKET_LEFT, HID_KEY_BRACKET_RIGHT, HID_KEY_BACKSLASH},

    // Row 3 (R3 - GPIO2): Home row
    {HID_KEY_CONTROL_LEFT, HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F, HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K, HID_KEY_L, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_NONE, HID_KEY_ENTER},

    // Row 4 (R4 - GPIO3): Bottom letter row
    {HID_KEY_SHIFT_LEFT, HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V, HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA, HID_KEY_PERIOD, HID_KEY_SLASH, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_SHIFT_RIGHT},

    // Row 5 (R5 - GPIO4): Bottom modifier row
    {HID_KEY_FN, HID_KEY_GUI_LEFT, HID_KEY_ALT_LEFT, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_SPACE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_ARROW_LEFT, HID_KEY_ARROW_DOWN, HID_KEY_ARROW_UP, HID_KEY_ARROW_RIGHT}
};

// FN Layer: When FN key is pressed
const uint8_t fn_keymap[NUM_ROWS][NUM_COLS] = {
    // Row 1 (R1 - GPIO0): Function keys
    {HID_KEY_ESCAPE, HID_KEY_F1, HID_KEY_F2, HID_KEY_F3, HID_KEY_F4, HID_KEY_F5, HID_KEY_F6, HID_KEY_F7, HID_KEY_F8, HID_KEY_F9, HID_KEY_F10, HID_KEY_F11, HID_KEY_F12, HID_KEY_DELETE},

    // Row 2 (R2 - GPIO1): Top letter row
    {HID_KEY_TAB, HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R, HID_KEY_T, HID_KEY_Y, HID_KEY_U, HID_KEY_I, HID_KEY_O, HID_KEY_P, HID_KEY_BRACKET_LEFT, HID_KEY_BRACKET_RIGHT, HID_KEY_BACKSLASH},

    // Row 3 (R3 - GPIO2): Home row
    {HID_KEY_CONTROL_LEFT, HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F, HID_KEY_G, HID_KEY_H, HID_KEY_J, HID_KEY_K, HID_KEY_L, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_NONE, HID_KEY_ENTER},

    // Row 4 (R4 - GPIO3): Bottom letter row
    {HID_KEY_SHIFT_LEFT, HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V, HID_KEY_B, HID_KEY_N, HID_KEY_M, HID_KEY_COMMA, HID_KEY_PERIOD, HID_KEY_SLASH, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_SHIFT_RIGHT},

    // Row 5 (R5 - GPIO4): Bottom modifier row
    {HID_KEY_FN, HID_KEY_GUI_LEFT, HID_KEY_ALT_LEFT, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_SPACE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_ARROW_LEFT, HID_KEY_ARROW_DOWN, HID_KEY_ARROW_UP, HID_KEY_ARROW_RIGHT}
};

// Check if FN key is pressed
bool is_fn_pressed(void)
{
    // FN key is at Row 5 (index 4), Column 1 (index 0)
    gpio_put(row_pins[4], 0);  // Set Row 5 LOW
    sleep_us(1);
    bool fn_pressed = !gpio_get(col_pins[0]);  // Check Column 1
    gpio_put(row_pins[4], 1);  // Set Row 5 back to HIGH
    return fn_pressed;
}

// Scan matrix and return pressed keycode (0 if no key pressed)
// 회로도: ROW -> 다이오드 -> 스위치 -> COLUMN
// 스캔 방식: ROW를 LOW로 설정하고 COLUMN을 읽음
uint8_t scan_matrix(void)
{
    bool fn_active = is_fn_pressed();
    const uint8_t (*active_keymap)[NUM_COLS] = fn_active ? fn_keymap : keymap;

    for (int row = 0; row < NUM_ROWS; row++) {
        // Set current row LOW
        gpio_put(row_pins[row], 0);
        sleep_us(1);  // Small delay for signal to stabilize

        // Read all columns
        for (int col = 0; col < NUM_COLS; col++) {
            bool is_pressed = !gpio_get(col_pins[col]);  // LOW = pressed

            if (is_pressed) {
                uint8_t keycode = active_keymap[row][col];
                // Set current row back to HIGH before returning
                gpio_put(row_pins[row], 1);

                // Don't return FN key itself
                if (keycode == HID_KEY_FN) {
                    continue;
                }

                return keycode;
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

    // Initialize row pins as outputs (ROW를 스캔)
    for (int i = 0; i < NUM_ROWS; i++) {
        gpio_init(row_pins[i]);
        gpio_set_dir(row_pins[i], GPIO_OUT);
        gpio_put(row_pins[i], 1);  // HIGH by default
    }

    // Initialize column pins as inputs with pull-up (COLUMN을 읽음)
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
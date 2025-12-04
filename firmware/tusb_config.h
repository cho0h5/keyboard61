#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT      0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUSB_MCU          OPT_MCU_RP2040
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#if defined(BOOTLOADER_BUILD)
  // Bootloader: CDC only
  #define CFG_TUD_HID             0
  #define CFG_TUD_CDC             1
#else
  // Main firmware: HID + CDC
  #define CFG_TUD_HID             1
  #define CFG_TUD_CDC             1
#endif

#define CFG_TUD_HID_EP_BUFSIZE    16
#define CFG_TUD_CDC_EP_BUFSIZE    64

// CDC buffer sizes (use defaults for endpoint size = 64B)
#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    256

#endif /* _TUSB_CONFIG_H_ */

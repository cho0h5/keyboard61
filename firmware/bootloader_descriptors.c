/*
 * Simple CDC-only descriptors for the bootloader
 */

#include <string.h>
#include "tusb.h"
#include "bsp/board_api.h"

#define USB_BCD   0x0200
#define USB_VID   0xCAFE
#define USB_PID   0x4001

enum
{
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82

char const *boot_string_desc[] =
{
  (const char[]){ 0x09, 0x04 }, // 0: English (0x0409)
  "Keyboard",                  // 1: Manufacturer
  "Keyboard Bootloader",       // 2: Product
  NULL,                        // 3: Serial (use unique if available)
};

enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
};

static uint16_t _desc_str[32 + 1];

tusb_desc_device_t const boot_desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const boot_desc_configuration[] =
{
  // Config number, interface count, string index, total length, attribute, power in mA
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

  // Interface number, string index, attributes, EP notification address and size, EP data OUT & IN address and size
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const * tud_descriptor_device_cb(void)
{
  return (uint8_t const *) &boot_desc_device;
}

uint8_t const * tud_descriptor_configuration_cb(uint8_t index)
{
  (void) index;
  return boot_desc_configuration;
}

uint16_t const * tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void) langid;
  size_t chr_count;

  switch ( index ) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], boot_string_desc[0], 2);
      chr_count = 1;
      break;

    case STRID_SERIAL:
      chr_count = board_usb_get_serial(_desc_str + 1, 32);
      break;

    default:
      if (!(index < sizeof(boot_string_desc) / sizeof(boot_string_desc[0]))) return NULL;

      const char *str = boot_string_desc[index];
      chr_count = strlen(str);
      size_t const max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
      if (chr_count > max_count) chr_count = max_count;
      for ( size_t i = 0; i < chr_count; i++ ) {
        _desc_str[1 + i] = str[i];
      }
      break;
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}

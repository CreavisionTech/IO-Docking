// USB descriptors for the IO Dock (single CDC device).
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

#define USBD_VID  0x1209u   // pid.codes vendor
#define USBD_PID  0x8888u

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_COUNT
};

static char serial_str[2 * 16 + 1]; // ASCII hex of the 64-bit unique id

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

// A single CDC presents exactly 2 interfaces (CDC control + CDC data), so the
// config descriptor must declare bNumInterfaces = 2. Passing STRID_COUNT here
// (5) made Windows fail enumeration with "Code 10: cannot start".
#define CDC_ITF_COUNT 2

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, CDC_ITF_COUNT, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, STRID_CDC, 0x81, 8, 0x02, 0x82, 64),
};

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 }, // 0: LANGID
    "IO Docking",                 // 1: Manufacturer
    "IO Dock RP2040",             // 2: Product
    serial_str,                   // 3: Serial
    "IO Dock CDC",                // 4: CDC interface
};

static uint16_t _desc_str[32 + 1];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

// Fill the serial string from the RP2040 unique 64-bit board id.
void board_serial_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (int i = 0; i < 8; i++) {
        sprintf(&serial_str[i * 2], "%02x", id.id[i]);
    }
    serial_str[16] = '\0';
}

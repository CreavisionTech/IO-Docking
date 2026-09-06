#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/*
 * TinyUSB configuration for the IO Dock firmware.
 * Device mode, single CDC (virtual serial) class.
 * CFG_TUSB_MCU / CFG_TUSB_OS are provided by the pico-sdk tinyusb integration.
 */

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)
#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  1024
#define CFG_TUD_CDC_TX_BUFSIZE  4096
#define CFG_TUD_CDC_EP_BUFSIZE  64

#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0
#define CFG_TUD_DFU             0

#endif

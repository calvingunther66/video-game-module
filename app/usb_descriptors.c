// Composite CDC (existing CLI) + HID (new keyboard/mouse injection)
// descriptor set. Structurally modeled on the vendored TinyUSB example at
// lib/pico_sdk/lib/tinyusb/examples/device/hid_composite/src/usb_descriptors.c
// and on the descriptor layout the Pico SDK's own (now-disabled, see
// tusb_config.h) pico_stdio_usb/stdio_usb_descriptors.c used for CDC-only.
//
// Because app/tusb_config.h + linking the marked `tinyusb_device` library
// (app/CMakeLists.txt) disables pico_stdio_usb's own tud_descriptor_*_cb()
// implementations (they're compiled out under `LIB_TINYUSB_DEVICE`), this
// file is now the *only* place providing them - there is no conflict/overlap
// with the SDK.

#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"
#include "usb_descriptors.h"

#ifndef USBD_VID
#define USBD_VID (0x2E8A) // Raspberry Pi
#endif

#ifndef USBD_PID
// Distinct from the stock pico_stdio_usb CDC-only PID (0x000a): this is now
// a composite CDC+HID device, and per USB conventions a device shouldn't
// reuse a PID advertising a different interface layout than what's actually
// enumerated (the host may cache/misapply a driver otherwise).
#define USBD_PID (0x000b)
#endif

#ifndef USBD_MANUFACTURER
#define USBD_MANUFACTURER "Flipper Devices Inc."
#endif

#ifndef USBD_PRODUCT
#define USBD_PRODUCT "VGM"
#endif

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Use IAD (Interface Association Descriptor) device class - required
    // since we expose multiple function-level interfaces (CDC needs it,
    // and it's harmless/expected for HID alongside it).
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 1,
};

const uint8_t* tud_descriptor_device_cb(void) {
    return (const uint8_t*)&usbd_desc_device;
}

//--------------------------------------------------------------------+
// HID Report Descriptor
//--------------------------------------------------------------------+

static const uint8_t usbd_desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
};

const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return usbd_desc_hid_report;
}

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+

enum {
    ITF_NUM_CDC = 0, // Also claims ITF_NUM_CDC + 1 (CDC data interface)
    ITF_NUM_HID = 2,
    ITF_NUM_CDC_DISPLAY = 3, // Also claims 4
    ITF_NUM_TOTAL = 5,
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_HID       0x83
#define EPNUM_DISP_NOTIF 0x84
#define EPNUM_DISP_OUT   0x05
#define EPNUM_DISP_IN    0x85

#define USBD_CDC_CMD_MAX_SIZE   (8)
#define USBD_CDC_IN_OUT_MAX_SIZE (64)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t usbd_desc_cfg[CONFIG_TOTAL_LEN] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 250),

    // CDC: Interface number, string index, EP notification address & size, EP data address (out, in) & size.
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC,
        4,
        EPNUM_CDC_NOTIF,
        USBD_CDC_CMD_MAX_SIZE,
        EPNUM_CDC_OUT,
        EPNUM_CDC_IN,
        USBD_CDC_IN_OUT_MAX_SIZE),

    // HID: Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(
        ITF_NUM_HID,
        5,
        HID_ITF_PROTOCOL_NONE,
        sizeof(usbd_desc_hid_report),
        EPNUM_HID,
        CFG_TUD_HID_EP_BUFSIZE,
        5),

    // Second CDC: desktop display stream (silent in non-desktop builds).
    TUD_CDC_DESCRIPTOR(
        ITF_NUM_CDC_DISPLAY,
        6,
        EPNUM_DISP_NOTIF,
        USBD_CDC_CMD_MAX_SIZE,
        EPNUM_DISP_OUT,
        EPNUM_DISP_IN,
        USBD_CDC_IN_OUT_MAX_SIZE),
};

const uint8_t* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usbd_desc_cfg;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+

#define USBD_STR_LANGID  0
#define USBD_STR_MANUF   1
#define USBD_STR_PRODUCT 2
#define USBD_STR_SERIAL  3
#define USBD_STR_CDC     4
#define USBD_STR_HID     5
#define USBD_STR_DISPLAY 6

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char* const usbd_desc_str[] = {
    [USBD_STR_MANUF] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_CDC] = "VGM CLI",
    [USBD_STR_HID] = "VGM HID",
    [USBD_STR_DISPLAY] = "VGM Display",
};

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

#define USBD_DESC_STR_MAX (20)
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    if(!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
    }

    uint8_t len;
    if(index == USBD_STR_LANGID) {
        desc_str[1] = 0x0409; // English
        len = 1;
    } else {
        if(index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) return NULL;
        const char* str = usbd_desc_str[index];
        if(!str) return NULL;
        for(len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            desc_str[1 + len] = (uint16_t)str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}

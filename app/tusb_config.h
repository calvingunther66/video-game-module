// Project-owned TinyUSB config - this file's directory is added to the
// include path AHEAD of the Pico SDK's own pico_stdio_usb tusb_config.h
// (see app/CMakeLists.txt's `target_include_directories(... BEFORE ...)`),
// so #include "tusb_config.h" from tusb.h resolves here instead.
//
// This makes the VGM's USB a composite CDC (existing CLI, unchanged) + HID
// (new - see usb_hid.c/usb_descriptors.c) device. Linking the *marked*
// `tinyusb_device` library (app/CMakeLists.txt) defines LIB_TINYUSB_DEVICE=1
// for this whole target, which is important: it's what causes the SDK's own
// pico_stdio_usb/stdio_usb_descriptors.c and tusb_config.h content to compile
// out (both are guarded on `!defined(LIB_TINYUSB_DEVICE)`), handing full
// control of the descriptors and the tud_init()/tud_task() lifecycle to us
// (see usb_hid.c) while pico_stdio_usb/stdio_usb.c still supplies the
// stdio_usb_in_chars()/out_chars() glue that the existing CLI (app/cli/)
// runs on top of via stdin/stdout - that part is untouched.
#pragma once

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)

// --- CDC (existing CLI-over-USB, see app/cli/) ---
// Keep enabled and unchanged in shape so pico_stdio_usb's CDC glue in
// stdio_usb.c (stdio_usb_in_chars/out_chars) keeps working exactly as before.
// Two CDC ports: 0 = CLI/stdio, 1 = "VGM Display" (desktop framebuffer
// stream + keyboard/mouse input, see desktop/usb_display.c).
#define CFG_TUD_CDC             (2)
#define CFG_TUD_CDC_RX_BUFSIZE  (256)
#define CFG_TUD_CDC_TX_BUFSIZE  (1024)

// --- HID (new - keyboard + mouse injection, see usb_hid.c) ---
#define CFG_TUD_HID             (1)
// Big enough for a boot keyboard report (8 bytes) or a mouse report (up to
// 5 bytes with wheel) - see usb_descriptors.c's report descriptors.
#define CFG_TUD_HID_EP_BUFSIZE  (16)

#define CFG_TUD_MSC             (0)
#define CFG_TUD_MIDI            (0)
#define CFG_TUD_VENDOR          (0)

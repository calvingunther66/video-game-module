#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the composite CDC+HID TinyUSB device stack (tud_init()) and
// starts the FreeRTOS task that pumps tud_task(). MUST be called before
// stdio_init_all() - see the comment above this call in main.c and in
// tusb_config.h for why (pico_stdio_usb's own stdio_usb_init(), invoked from
// stdio_init_all(), asserts tud_inited() once LIB_TINYUSB_DEVICE is defined).
void usb_hid_init(void);

// Keyboard/mouse report submission, called from coproc.c while decoding a
// COPROC_TASK_HID_INJECT job. keycode is a USB HID Keyboard/Keypad usage ID
// (see hid.h's hid_keyboard_keycode_t), not ASCII. All of these block for a
// short, bounded time waiting for the HID IN endpoint to be free, and return
// false (without retrying further) if it doesn't free up in time - the
// caller/job just reports that key/click as failed rather than hanging.
bool usb_hid_key_press(uint16_t keycode);
bool usb_hid_key_release(uint16_t keycode);

// button: 0=left, 1=right, 2=middle (matches HID Button usage 1/2/3 - 1).
bool usb_hid_mouse_move(int8_t dx, int8_t dy);
bool usb_hid_mouse_click(uint8_t button, bool down);
bool usb_hid_mouse_scroll(int8_t delta);

// Releases all held keyboard keys and mouse buttons - panic/safety reset,
// also used defensively at the start of a fresh HID-inject job.
bool usb_hid_release_all(void);

#ifdef __cplusplus
}
#endif

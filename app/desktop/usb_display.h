#pragma once
/*
 * "VGM Display": streams the desktop framebuffer out of the VGM's own USB-C
 * port (second CDC interface) and takes keyboard/mouse input back, so a
 * computer can act as the desktop's screen and keyboard.
 *
 * Wire format, both directions: 0xA5 0x5A type:u8 len:u16le payload[len]
 *
 * Device -> host
 *   0x01 HELLO   w:u16 h:u16 n:u16 palette[n] (RGB888, 3 bytes each)
 *   0x02 ROW     y:u16 then RLE pairs (count:u8 1..255, index:u8) = w pixels
 *   0x03 FRAME   seq:u16   (end of one scan; only changed rows were sent)
 *
 * Host -> device
 *   0x10 REFRESH            resend HELLO and every row
 *   0x11 KEY     key:u8 flags:u8   (USB_DISPLAY_KEY_*, flags bit0 = long)
 *   0x12 CLICK   x:u16 y:u16 button:u8   (framebuffer coordinates)
 *   0x13 WHEEL   delta:i8              (+1 = down one row)
 */
#include <stdint.h>

#define USB_DISPLAY_CDC 1

enum {
    USB_DISPLAY_KEY_UP = 1,
    USB_DISPLAY_KEY_DOWN,
    USB_DISPLAY_KEY_LEFT,
    USB_DISPLAY_KEY_RIGHT,
    USB_DISPLAY_KEY_ENTER,
    USB_DISPLAY_KEY_ESC,
    USB_DISPLAY_KEY_TAB,
    USB_DISPLAY_KEY_BACKSPACE,
    USB_DISPLAY_KEY_SHIFT_TAB,
};

/* Call from the task that runs tud_task(), after each tud_task(). */
void usb_display_service(void);

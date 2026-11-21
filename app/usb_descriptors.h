#pragma once

#include <stdint.h>

// HID report IDs - both the keyboard and mouse reports live on the same HID
// interface (see usb_descriptors.c), distinguished by these IDs. Boot-style
// reports: keyboard is [modifier][reserved][keycode x6], mouse is
// [buttons][x][y][wheel].
enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_MOUSE = 2,
};

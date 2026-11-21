// TinyUSB HID keyboard/mouse report submission. Driven from coproc.c while
// decoding a COPROC_TASK_HID_INJECT job (fire-and-forget actuation - see
// coproc.h). See tusb_config.h and app/CMakeLists.txt for how this coexists
// with the existing CDC-based CLI as one composite USB device.

#include "usb_hid.h"
#include "usb_descriptors.h"

#include <string.h>
#include "tusb.h"
#include <FreeRTOS.h>
#include <task.h>

#define HID_TASK_STACK_SIZE_WORDS 1024
// Bounded wait for the HID IN endpoint to free up between reports. A few ms
// is normally enough (host polls at the 5ms interval configured in
// usb_descriptors.c); this just keeps a stuck/unplugged host target from
// hanging the caller (coproc's compute_task) indefinitely.
#define HID_REPORT_READY_TIMEOUT_MS 20

// Keyboard/mouse "currently held" state, so e.g. usb_hid_mouse_click() can
// send a report reflecting all held buttons, not just the one that changed,
// and so usb_hid_release_all() has something to clear.
static uint8_t kb_modifier;
static uint16_t kb_keys[6]; // non-modifier keycodes; 0 = empty slot
static uint8_t mouse_buttons; // bit0=left, bit1=right, bit2=middle

static bool hid_wait_ready(void) {
    // Report calls (this file, called from coproc's compute_task) and
    // tud_task() (usb_hid_task, below) run on different FreeRTOS tasks with
    // no shared lock - that's the standard supported TinyUSB pattern (see
    // e.g. TinyUSB's own cdc_msc_freertos example), not a race.
    for(uint32_t waited_ms = 0; waited_ms < HID_REPORT_READY_TIMEOUT_MS; waited_ms++) {
        if(tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return tud_hid_ready();
}

static bool hid_send_keyboard_report(void) {
    if(!hid_wait_ready()) return false;
    uint8_t keycodes[6];
    for(int i = 0; i < 6; i++) keycodes[i] = (uint8_t)kb_keys[i];
    return tud_hid_keyboard_report(REPORT_ID_KEYBOARD, kb_modifier, keycodes);
}

static bool hid_send_mouse_report(int8_t dx, int8_t dy, int8_t wheel) {
    if(!hid_wait_ready()) return false;
    return tud_hid_mouse_report(REPORT_ID_MOUSE, mouse_buttons, dx, dy, wheel, 0);
}

// Usage IDs 0xE0-0xE7 (224-231) on the Keyboard/Keypad page are the
// modifier keys (L/R Ctrl, Shift, Alt, GUI) - boot keyboard reports carry
// those as a bitmask, not as keycode-array entries.
static inline bool is_modifier_keycode(uint16_t keycode) {
    return keycode >= 0xE0 && keycode <= 0xE7;
}

bool usb_hid_key_press(uint16_t keycode) {
    if(is_modifier_keycode(keycode)) {
        kb_modifier |= (uint8_t)(1u << (keycode - 0xE0));
        return hid_send_keyboard_report();
    }

    for(int i = 0; i < 6; i++) {
        if(kb_keys[i] == keycode) return true; // already held
    }
    for(int i = 0; i < 6; i++) {
        if(kb_keys[i] == 0) {
            kb_keys[i] = keycode;
            return hid_send_keyboard_report();
        }
    }
    // All 6 rollover slots full. Real HID boot-keyboard behavior would send
    // a "phantom state" report (all keycodes = 1); simplest safe behavior
    // here is to just report the failure back up to the job.
    return false;
}

bool usb_hid_key_release(uint16_t keycode) {
    if(is_modifier_keycode(keycode)) {
        kb_modifier &= (uint8_t)~(1u << (keycode - 0xE0));
        return hid_send_keyboard_report();
    }

    for(int i = 0; i < 6; i++) {
        if(kb_keys[i] == keycode) {
            kb_keys[i] = 0;
            return hid_send_keyboard_report();
        }
    }
    return true; // wasn't held - nothing to do, not an error
}

bool usb_hid_mouse_move(int8_t dx, int8_t dy) {
    return hid_send_mouse_report(dx, dy, 0);
}

bool usb_hid_mouse_click(uint8_t button, bool down) {
    if(button > 2) return false;
    if(down) {
        mouse_buttons |= (uint8_t)(1u << button);
    } else {
        mouse_buttons &= (uint8_t)~(1u << button);
    }
    return hid_send_mouse_report(0, 0, 0);
}

bool usb_hid_mouse_scroll(int8_t delta) {
    return hid_send_mouse_report(0, 0, delta);
}

bool usb_hid_release_all(void) {
    kb_modifier = 0;
    memset(kb_keys, 0, sizeof(kb_keys));
    mouse_buttons = 0;
    bool kb_ok = hid_send_keyboard_report();
    bool mouse_ok = hid_send_mouse_report(0, 0, 0);
    return kb_ok && mouse_ok;
}

//--------------------------------------------------------------------+
// Mandatory (non-weak) TinyUSB HID callbacks. The VGM has no GET_REPORT
// data to offer and ignores SET_REPORT (e.g. host->device keyboard LED
// state) from the host - both are legitimate no-ops for an
// injection-only HID device.
//--------------------------------------------------------------------+

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t* buffer,
    uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const* buffer,
    uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

//--------------------------------------------------------------------+
// Device stack lifecycle
//--------------------------------------------------------------------+

static void usb_hid_task(void* unused_arg) {
    (void)unused_arg;
    for(;;) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void usb_hid_init(void) {
    // RP2040 has exactly one USB device-capable port.
    tud_init(0);

    TaskHandle_t task_handle = NULL;
    BaseType_t status = xTaskCreate(
        usb_hid_task, "usb_hid_task", HID_TASK_STACK_SIZE_WORDS, NULL, 1, &task_handle);
    configASSERT(status == pdPASS);
    (void)status;
}

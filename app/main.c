#include <stdio.h>
#include <stdlib.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>
#include <pico/sem.h>
#include <FreeRTOS.h>
#include <task.h>
#include "frame.h"
#include "led.h"
#include "led_state.h"
#include "uart.h"
#include "usb.h"
#include "usb_hid.h"
#include "bitmaps.h"
#include "coproc.h"
#ifdef VGM_DESKTOP
#include "desktop/desktop_task.h"
#endif

static void init() {
    led_init();

    sleep_ms(10);
    vreg_set_voltage(frame_get_voltage());

    sleep_ms(10);
    set_sys_clock_khz(frame_get_clock(), true);

    // Must run before stdio_init_all(): this brings up the shared TinyUSB
    // device stack (tud_init()). USB is now a composite CDC+HID device (see
    // tusb_config.h) rather than pico_stdio_usb's stock CDC-only setup, and
    // pico_stdio_usb's own stdio_usb_init() (called from stdio_init_all()
    // below) asserts tud_inited() instead of calling tusb_init() itself once
    // it detects that - see stdio_usb.c.
    usb_hid_init();

    stdio_init_all();
#ifdef VGM_DESKTOP
    // Before frame_init(): core 1 starts scanning out gfx_fb immediately.
    desktop_task_init();
#endif
    frame_init();
    usb_init();
    uart_protocol_init();
    coproc_init();

    led_red(false);
}

#ifndef VGM_DESKTOP
static void show_defaul_screen() {
    uint8_t* frame_buffer = calloc(FLIPPER_BITMAP_SIZE, sizeof(uint8_t));

    bitmap_xbm_to_screen_frame(
        frame_buffer, bitmap_default_screen, FLIPPER_SCREEN_WIDTH, FLIPPER_SCREEN_HEIGHT);
    frame_parse_data(OrientationHorizontal, (const frame_t*)frame_buffer, pdMS_TO_TICKS(1000));

    free(frame_buffer);
}
#endif

int main() {
    init();

#ifndef VGM_DESKTOP
    show_defaul_screen();
#endif

    // init the led task
    led_state_init();

    vTaskStartScheduler();

    while(1) __wfe();
    __builtin_unreachable();
}

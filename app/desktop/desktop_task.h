#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Call after frame_init() and before the scheduler starts. */
void desktop_task_init(void);

/* A Flipper screen frame (1024 B, 8 pages x 128 columns) to show. */
void desktop_task_set_flipper_frame(uint8_t orientation, const uint8_t* data);

typedef enum {
    DESKTOP_INPUT_KEY,
    DESKTOP_INPUT_CLICK,
    DESKTOP_INPUT_WHEEL,
} desktop_input_type_t;

typedef struct {
    desktop_input_type_t type;
    uint8_t key; /* USB_DISPLAY_KEY_* */
    bool long_press;
    int16_t x, y;
    int8_t delta;
} desktop_input_t;

/* Thread-safe; drops the event if the queue is full. */
void desktop_task_post_input(const desktop_input_t* input);

/* A remote screen/keyboard (USB display viewer) connected or went away. */
void desktop_task_set_remote(bool connected);

#pragma once
#include <stdbool.h>
#include <stdint.h>

void uart_protocol_init(void);

/*
 * Requests the VGM sends to the Flipper over the live RPC session, in
 * between the screen frames the Flipper streams to us. Submitting never
 * blocks; requests queued while the link is down are dropped on reconnect.
 */
typedef enum {
    LinkRequestPing,
    LinkRequestInput, /* PB_Gui_InputKey / PB_Gui_InputType */
    LinkRequestDateTime,
    LinkRequestPowerInfo,
    LinkRequestStorageList,
    LinkRequestAppStart, /* app.name = loader app name or .fap path, app.args */
} LinkRequestType;

#define LINK_PATH_MAX 96
#define LINK_APP_NAME_MAX 64
#define LINK_APP_ARGS_MAX 168

typedef struct {
    LinkRequestType type;
    union {
        struct {
            uint8_t key;
            uint8_t type;
        } input;
        char path[LINK_PATH_MAX];
        struct {
            char name[LINK_APP_NAME_MAX];
            char args[LINK_APP_ARGS_MAX];
        } app;
    };
} LinkRequest;

bool uart_link_submit(const LinkRequest* request);
bool uart_link_is_up(void);

/*
 * Answers to requests, and link state changes. The listener runs on the
 * UART task: copy what you need and return quickly. Strings are only valid
 * during the call.
 */
typedef enum {
    LinkEventUp,
    LinkEventDown,
    LinkEventDateTime,
    LinkEventPower, /* one key/value pair per event, e.g. charge_level=76 */
    LinkEventListEntry,
    LinkEventListDone,
    LinkEventListError,
} LinkEventType;

typedef struct {
    LinkEventType type;
    union {
        struct {
            uint8_t hour;
            uint8_t minute;
        } datetime;
        struct {
            const char* key;
            const char* value;
        } power;
        struct {
            const char* name;
            bool is_dir;
            uint32_t size;
        } entry;
    };
} LinkEvent;

typedef void (*LinkListener)(const LinkEvent* event);
void uart_link_set_listener(LinkListener listener);

/*
 * Screen frames from the Flipper. The default (weak) implementation mirrors
 * them full screen via frame_parse_data(); the desktop build overrides it.
 */
void uart_link_on_screen_frame(uint8_t orientation, const uint8_t* data);

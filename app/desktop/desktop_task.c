/*
 * Firmware glue for the desktop shell (VGM_DESKTOP builds only): owns the
 * desktop state, turns link events and Flipper screen frames into state
 * changes, and redraws the 8bpp framebuffer that core 1 scans out.
 */
#include "desktop_task.h"

#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "desktop.h"
#include "uart.h"
#include "usb_display.h"

#define DESKTOP_TASK_STACK_WORDS (1024)
#define DESKTOP_CLOCK_POLL_MS (10000)
#define DESKTOP_POWER_POLL_MS (30000)

#define DIRTY_FULL (1u << 0)
#define DIRTY_FLIPPER (1u << 1)
#define LINK_CAME_UP (1u << 2)
#define INPUT_PENDING (1u << 3)
#define INPUT_QUEUE_LEN 16

/* PB_Gui_InputKey / PB_Gui_InputType values (gui.pb.h). */
#define PB_KEY_UP 0
#define PB_KEY_DOWN 1
#define PB_KEY_RIGHT 2
#define PB_KEY_LEFT 3
#define PB_KEY_OK 4
#define PB_KEY_BACK 5
#define PB_INPUT_PRESS 0
#define PB_INPUT_RELEASE 1
#define PB_INPUT_SHORT 2
#define PB_INPUT_LONG 3

static desktop_state_t state;
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static QueueHandle_t input_queue;

static void notify(uint32_t bits) {
    if(task) xTaskNotify(task, bits, eSetBits);
}

static void submit_simple(LinkRequestType type) {
    LinkRequest request = {.type = type};
    uart_link_submit(&request);
}

static void submit_list(const char* path) {
    LinkRequest request = {.type = LinkRequestStorageList};
    snprintf(request.path, sizeof(request.path), "%s", path);
    uart_link_submit(&request);
}

void desktop_task_set_flipper_frame(uint8_t orientation, const uint8_t* data) {
    if(!lock) return;
    if(orientation > 3) orientation = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool first = !state.have_frame;
    memcpy(state.frame, data, sizeof(state.frame));
    state.frame_orientation = orientation;
    state.have_frame = true;
    xSemaphoreGive(lock);
    notify(first ? DIRTY_FULL : DIRTY_FLIPPER);
}

static bool is_root(const char* path) {
    return strcmp(path, "/ext") == 0 || strcmp(path, "/") == 0;
}

static int file_cmp(const dt_file_t* a, const dt_file_t* b) {
    if(strcmp(a->name, "..") == 0) return -1;
    if(strcmp(b->name, "..") == 0) return 1;
    if(a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void sort_files(void) {
    for(int i = 1; i < state.file_count; i++) {
        dt_file_t tmp = state.files[i];
        int j = i - 1;
        while(j >= 0 && file_cmp(&state.files[j], &tmp) > 0) {
            state.files[j + 1] = state.files[j];
            j--;
        }
        state.files[j + 1] = tmp;
    }
}

static void add_file(const char* name, bool is_dir, uint32_t size) {
    if(state.file_count >= DT_MAX_FILES) {
        state.files_truncated = true;
        return;
    }
    dt_file_t* f = &state.files[state.file_count++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->is_dir = is_dir;
    f->size = size;
}

static void on_link_event(const LinkEvent* event) {
    uint32_t bits = DIRTY_FULL;
    xSemaphoreTake(lock, portMAX_DELAY);
    switch(event->type) {
    case LinkEventUp:
        state.link_up = true;
        bits |= LINK_CAME_UP;
        break;
    case LinkEventDown:
        state.link_up = false;
        state.have_frame = false;
        state.battery_pct = -1;
        state.charging = false;
        state.battery_health_pct = -1;
        state.battery_cycles = -1;
        state.clock[0] = 0;
        state.sd_present = false;
        state.file_count = 0;
        state.files_loading = false;
        break;
    case LinkEventDateTime:
        snprintf(state.clock, sizeof(state.clock), "%02u:%02u", event->datetime.hour % 24u,
                 event->datetime.minute % 60u);
        break;
    case LinkEventPower:
        if(strcmp(event->power.key, "charge_level") == 0) {
            state.battery_pct = atoi(event->power.value);
        } else if(strcmp(event->power.key, "charge_state") == 0) {
            state.charging = strcmp(event->power.value, "charging") == 0;
        } else if(strcmp(event->power.key, "battery_health") == 0) {
            state.battery_health_pct = atoi(event->power.value);
        } else if(strcmp(event->power.key, "battery_cycles") == 0) {
            /* Sent since Flipware power info format 2.2; absent on older firmware. */
            state.battery_cycles = atoi(event->power.value);
        } else {
            bits = 0; // other keys don't change the screen
        }
        break;
    case LinkEventListEntry:
        // Hide dot entries (.fseventsd, .Spotlight-V100, .tmp, ...), like the
        // Flipper's own file browser does.
        if(event->entry.name[0] != '.')
            add_file(event->entry.name, event->entry.is_dir, event->entry.size);
        bits = 0; // redraw once the listing is complete
        break;
    case LinkEventListDone:
        state.files_loading = false;
        state.sd_present = true;
        sort_files();
        desktop_files_clamp(&state);
        break;
    case LinkEventListError:
        state.files_loading = false;
        state.file_count = 0;
        if(is_root(state.cwd)) state.sd_present = false;
        snprintf(state.status, sizeof(state.status), "Cannot open %.48s", state.cwd);
        break;
    }
    xSemaphoreGive(lock);
    if(bits) notify(bits);
}

/* Called with the lock held. */
static void open_dir_locked(const char* path) {
    if(path != state.cwd) snprintf(state.cwd, sizeof(state.cwd), "%s", path);
    state.file_count = 0;
    state.file_sel = 0;
    state.file_scroll = 0;
    state.files_truncated = false;
    state.files_loading = true;
    if(!is_root(path)) add_file("..", true, 0);
    submit_list(path);
}

/* ---- input (USB display viewer; later a USB keyboard) ---- */


void desktop_task_post_input(const desktop_input_t* input) {
    if(!input_queue) return;
    if(xQueueSend(input_queue, input, 0) == pdTRUE) notify(INPUT_PENDING);
}

void desktop_task_set_remote(bool connected) {
    if(!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    state.input_ready = connected;
    xSemaphoreGive(lock);
    notify(DIRTY_FULL);
}

static void send_flipper_key(uint8_t key, bool long_press) {
    LinkRequest r = {.type = LinkRequestInput};
    r.input.key = key;
    r.input.type = PB_INPUT_PRESS;
    uart_link_submit(&r);
    r.input.type = long_press ? PB_INPUT_LONG : PB_INPUT_SHORT;
    uart_link_submit(&r);
    r.input.type = PB_INPUT_RELEASE;
    uart_link_submit(&r);
}

static void set_status_locked(const char* text) {
    snprintf(state.status, sizeof(state.status), "%s", text);
}

/*
 * File associations, the same ones the Flipper's Archive app uses
 * (archive_get_flipper_app_name). The app gets the file path as its args;
 * a .fap is started by its own path. Firmware update manifests (.fuf) are
 * left out on purpose: no updates one Enter away from the desktop.
 * RPC input is tagged as software, so the Flipper doesn't remap keys for
 * rotated screens and arrows stay in the logical directions we draw.
 */
typedef struct {
    const char* ext;
    const char* app;
} dt_assoc_t;

static const dt_assoc_t file_assoc[] = {
    {".sub", "Sub-GHz"},
    {".nfc", "NFC"},
    {".rfid", "125 kHz RFID"},
    {".ir", "Infrared"},
    {".ibtn", "iButton"},
    {".js", "/ext/apps/assets/js_app.fap"}, /* JS Runner is a FAP in Momentum */
    {".wav", "/ext/apps/Media/wav_player.fap"},
    {".mag", "/ext/apps/GPIO/magspoof.fap"},
    {".xr", "/ext/apps/Infrared/xremote.fap"},
    {".picopass", "/ext/apps/NFC/picopass.fap"},
    {".psf", "/ext/apps/Sub-Ghz/proto_pirate.fap"},
    {".img", "/ext/apps/USB/mass_storage.fap"},
};

/* Folder-dependent .txt files, as in archive_files.c; others open as text. */
static const dt_assoc_t txt_assoc[] = {
    {"/ext/badusb", "Bad KB"},
    {"/ext/subghz/playlist", "/ext/apps/Sub-Ghz/subghz_playlist.fap"},
    {"/ext/subghz/remote", "/ext/apps/Sub-Ghz/subghz_remote.fap"},
    {"/ext/infrared/remote", "/ext/apps/Infrared/ir_remote.fap"},
};

/* path is prefix itself or below it. */
static bool in_folder(const char* path, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == 0 || path[n] == '/');
}

static bool start_app_locked(const char* app, const char* args, const char* label);

/* Called with the lock held. */
static void open_file_locked(const char* name) {
    char path[LINK_APP_ARGS_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", state.cwd, name);
    if(n <= 0 || n >= (int)sizeof(path)) {
        set_status_locked("Path too long");
        return;
    }
    const char* ext = strrchr(name, '.');
    if(!ext) ext = "";

    if(strcasecmp(ext, ".fap") == 0) {
        if(n >= LINK_APP_NAME_MAX) {
            set_status_locked("Path too long");
            return;
        }
        start_app_locked(path, NULL, name);
        return;
    }
    if(strcasecmp(ext, ".txt") == 0) {
        for(size_t i = 0; i < sizeof(txt_assoc) / sizeof(txt_assoc[0]); i++) {
            if(in_folder(state.cwd, txt_assoc[i].ext)) {
                start_app_locked(txt_assoc[i].app, path, name);
                return;
            }
        }
        start_app_locked("/ext/apps/Tools/text_viewer.fap", path, name);
        return;
    }
    for(size_t i = 0; i < sizeof(file_assoc) / sizeof(file_assoc[0]); i++) {
        if(strcasecmp(ext, file_assoc[i].ext) == 0) {
            start_app_locked(file_assoc[i].app, path, name);
            return;
        }
    }
    snprintf(state.status, sizeof(state.status), "No app for %.40s files", ext[0] ? ext : "these");
}

/* Called with the lock held. */
static void parent_dir_locked(void) {
    if(is_root(state.cwd)) return;
    char* slash = strrchr(state.cwd, '/');
    if(slash && slash != state.cwd) *slash = 0;
    open_dir_locked(state.cwd);
}

/* Called with the lock held. */
static void activate_file_locked(void) {
    if(state.files_loading || state.file_sel >= state.file_count) return;
    const dt_file_t* f = &state.files[state.file_sel];
    if(strcmp(f->name, "..") == 0) {
        parent_dir_locked();
    } else if(f->is_dir) {
        char path[DT_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", state.cwd, f->name);
        if(n > 0 && n < (int)sizeof(path))
            open_dir_locked(path);
        else
            set_status_locked("Path too long");
    } else {
        open_file_locked(f->name);
    }
}

/* Called with the lock held. */
static bool start_app_locked(const char* app, const char* args, const char* label) {
    LinkRequest r = {.type = LinkRequestAppStart};
    snprintf(r.app.name, sizeof(r.app.name), "%.*s", (int)sizeof(r.app.name) - 1, app);
    snprintf(r.app.args, sizeof(r.app.args), "%s", args ? args : "");
    if(!uart_link_submit(&r)) {
        set_status_locked("Flipper not linked");
        return false;
    }
    snprintf(state.status, sizeof(state.status), "Starting %.50s", label);
    state.focus = DT_FOCUS_FLIPPER;
    return true;
}

/* Called with the lock held. */
static void launch_dock_locked(void) {
    const dt_dock_item_t* it = &desktop_dock_items[state.dock_sel];
    start_app_locked(it->app, NULL, it->label);
}

/* Called with the lock held. */
static void handle_key_locked(uint8_t key, bool long_press) {
    state.status[0] = 0;
    if(key == USB_DISPLAY_KEY_TAB) {
        state.focus = (state.focus + 1) % DT_FOCUS_COUNT;
        return;
    }
    if(key == USB_DISPLAY_KEY_SHIFT_TAB) {
        state.focus = (state.focus + DT_FOCUS_COUNT - 1) % DT_FOCUS_COUNT;
        return;
    }
    switch(state.focus) {
    case DT_FOCUS_FLIPPER: {
        static const int8_t map[] = {
            [USB_DISPLAY_KEY_UP] = PB_KEY_UP,
            [USB_DISPLAY_KEY_DOWN] = PB_KEY_DOWN,
            [USB_DISPLAY_KEY_LEFT] = PB_KEY_LEFT,
            [USB_DISPLAY_KEY_RIGHT] = PB_KEY_RIGHT,
            [USB_DISPLAY_KEY_ENTER] = PB_KEY_OK,
            [USB_DISPLAY_KEY_ESC] = PB_KEY_BACK,
            [USB_DISPLAY_KEY_BACKSPACE] = PB_KEY_BACK,
        };
        if(key < sizeof(map) && (key == USB_DISPLAY_KEY_UP || map[key] != 0))
            send_flipper_key((uint8_t)map[key], long_press);
        break;
    }
    case DT_FOCUS_FILES:
        if(key == USB_DISPLAY_KEY_UP) state.file_sel--;
        else if(key == USB_DISPLAY_KEY_DOWN) state.file_sel++;
        else if(key == USB_DISPLAY_KEY_LEFT) state.file_sel -= DT_FILES_VISIBLE;
        else if(key == USB_DISPLAY_KEY_RIGHT) state.file_sel += DT_FILES_VISIBLE;
        else if(key == USB_DISPLAY_KEY_ENTER) activate_file_locked();
        else if(key == USB_DISPLAY_KEY_BACKSPACE || key == USB_DISPLAY_KEY_ESC)
            parent_dir_locked();
        desktop_files_clamp(&state);
        break;
    case DT_FOCUS_DOCK:
        if(key == USB_DISPLAY_KEY_UP && state.dock_sel > 0) state.dock_sel--;
        else if(key == USB_DISPLAY_KEY_DOWN && state.dock_sel < desktop_dock_count - 1)
            state.dock_sel++;
        else if(key == USB_DISPLAY_KEY_ENTER) launch_dock_locked();
        break;
    default:
        break;
    }
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* Called with the lock held. Click selects; clicking the selection opens it. */
static void handle_click_locked(int x, int y) {
    state.status[0] = 0;
    if(inside(x, y, FLIP_X, FLIP_Y, FLIP_W, FLIP_H)) {
        state.focus = DT_FOCUS_FLIPPER;
    } else if(inside(x, y, DOCK_X, DOCK_Y, DOCK_W, FLIP_H)) {
        int i = (y - (DOCK_Y + TITLE_H + 2)) / DOCK_TILE_H;
        bool was = state.focus == DT_FOCUS_DOCK;
        state.focus = DT_FOCUS_DOCK;
        if(y >= DOCK_Y + TITLE_H + 2 && i >= 0 && i < desktop_dock_count) {
            if(was && i == state.dock_sel)
                launch_dock_locked();
            else
                state.dock_sel = i;
        }
    } else if(inside(x, y, FILES_X, FILES_Y, FILES_W, FILES_H)) {
        bool was = state.focus == DT_FOCUS_FILES;
        state.focus = DT_FOCUS_FILES;
        int top = FILES_Y + TITLE_H + 1;
        if(y >= top) {
            int i = state.file_scroll + (y - top) / FILES_ROW_H;
            if(i < state.file_count) {
                if(was && i == state.file_sel)
                    activate_file_locked();
                else
                    state.file_sel = i;
            }
        }
    }
}

static void handle_inputs(void) {
    desktop_input_t in;
    bool any = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    while(xQueueReceive(input_queue, &in, 0) == pdTRUE) {
        any = true;
        if(in.type == DESKTOP_INPUT_KEY) {
            handle_key_locked(in.key, in.long_press);
        } else if(in.type == DESKTOP_INPUT_CLICK) {
            handle_click_locked(in.x, in.y);
        } else if(in.type == DESKTOP_INPUT_WHEEL && state.file_count > 0) {
            state.file_scroll += in.delta;
            int max = state.file_count - DT_FILES_VISIBLE;
            if(state.file_scroll > max) state.file_scroll = max;
            if(state.file_scroll < 0) state.file_scroll = 0;
            if(state.file_sel < state.file_scroll) state.file_sel = state.file_scroll;
            if(state.file_sel >= state.file_scroll + DT_FILES_VISIBLE)
                state.file_sel = state.file_scroll + DT_FILES_VISIBLE - 1;
        }
    }
    if(any) desktop_render(&state);
    xSemaphoreGive(lock);
}

static void desktop_task(void* arg) {
    (void)arg;
    TickType_t last_clock = 0, last_power = 0;
    uint32_t bits = DIRTY_FULL;

    while(true) {
        const TickType_t now = xTaskGetTickCount();
        bool link_up;

        if(bits & INPUT_PENDING) handle_inputs();

        xSemaphoreTake(lock, portMAX_DELAY);
        link_up = state.link_up;
        if(bits & LINK_CAME_UP) {
            open_dir_locked(state.cwd);
            last_clock = last_power = 0;
        }
        if(bits & (DIRTY_FULL | LINK_CAME_UP)) {
            desktop_render(&state);
        } else if(bits & DIRTY_FLIPPER) {
            desktop_render_flipper(&state);
        }
        xSemaphoreGive(lock);

        if(link_up) {
            if(last_clock == 0 || now - last_clock >= pdMS_TO_TICKS(DESKTOP_CLOCK_POLL_MS)) {
                submit_simple(LinkRequestDateTime);
                last_clock = now ? now : 1;
            }
            if(last_power == 0 || now - last_power >= pdMS_TO_TICKS(DESKTOP_POWER_POLL_MS)) {
                submit_simple(LinkRequestPowerInfo);
                last_power = now ? now : 1;
            }
        }

        bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(1000));
    }
}

void desktop_task_init(void) {
    desktop_init_palette();
    desktop_state_init(&state);
    desktop_render(&state);

    // configUSE_MUTEXES is 0 in this firmware; a binary semaphore works as a
    // lock between two equal-priority tasks.
    input_queue = xQueueCreate(INPUT_QUEUE_LEN, sizeof(desktop_input_t));
    configASSERT(input_queue);
    lock = xSemaphoreCreateBinary();
    configASSERT(lock);
    xSemaphoreGive(lock);
    uart_link_set_listener(on_link_event);
    BaseType_t ok =
        xTaskCreate(desktop_task, "desktop", DESKTOP_TASK_STACK_WORDS, NULL, 1, &task);
    configASSERT(ok == pdPASS);
    (void)ok;
}

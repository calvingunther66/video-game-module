#pragma once
/*
 * Flipper Desktop: the shell the VGM draws on HDMI instead of a bare
 * upscaled Flipper screen. Pure rendering from a state struct; the firmware
 * glue (desktop_task.c) owns the state and decides when to redraw.
 */
#include "gfx.h"

/* Palette indices. desktop_init_palette() loads the colours. */
enum {
    DT_C_BG = 0,
    DT_C_BG_DOT,
    DT_C_PANEL,
    DT_C_PANEL_ALT,
    DT_C_BORDER,
    DT_C_SHADOW,
    DT_C_TEXT,
    DT_C_TEXT_DIM,
    DT_C_ACCENT,
    DT_C_ACCENT_DIM,
    DT_C_ON_ACCENT,
    DT_C_LCD_BG,
    DT_C_LCD_FG,
    DT_C_OK,
    DT_C_WARN,
    DT_C_LCD_OFF,
    DT_C_COUNT,
};

typedef enum {
    DT_FOCUS_FLIPPER = 0,
    DT_FOCUS_FILES,
    DT_FOCUS_DOCK,
    DT_FOCUS_COUNT,
} dt_focus_t;

typedef struct {
    char name[64];
    bool is_dir;
    uint32_t size;
} dt_file_t;

#define DT_MAX_FILES 48
#define DT_PATH_MAX 96
#define DT_FILES_VISIBLE 4

/* Layout (framebuffer pixels); also used to hit-test mouse clicks. */
#define TOPBAR_H 14
#define TITLE_H 12
#define FLIP_SCALE 2
#define FLIP_X 6
#define FLIP_Y (TOPBAR_H + 4)
#define FLIP_W (128 * FLIP_SCALE + 6)
#define FLIP_H (TITLE_H + 64 * FLIP_SCALE + 5)
#define LCD_X (FLIP_X + 3)
#define LCD_Y (FLIP_Y + TITLE_H + 2)
#define DOCK_X (FLIP_X + FLIP_W + 6)
#define DOCK_Y FLIP_Y
#define DOCK_W (GFX_W - DOCK_X - 6)
#define DOCK_H FLIP_H
#define DOCK_TILE_H 21
#define FILES_X 6
#define FILES_Y (FLIP_Y + FLIP_H + 4)
#define FILES_W (GFX_W - 12)
#define FILES_ROW_H 11
#define FILES_H (TITLE_H + DT_FILES_VISIBLE * FILES_ROW_H + 2)
#define HINT_Y (GFX_H - 12)

typedef struct {
    const char* label;
    const gfx_icon_t* icon;
    const char* app; /* loader name for RPC app_start */
} dt_dock_item_t;

extern const dt_dock_item_t desktop_dock_items[];
extern const int desktop_dock_count;

typedef struct {
    /* Link and Flipper screen */
    bool link_up;
    bool have_frame;
    uint8_t frame_orientation; /* PB_Gui_ScreenOrientation */
    uint8_t frame[1024];

    /* Top bar; battery_pct < 0 means unknown, clock[0] == 0 means unknown */
    int battery_pct;
    bool charging;
    bool sd_present;
    char clock[6];

    dt_focus_t focus;
    bool input_ready; /* a keyboard is attached */

    /* File manager */
    char cwd[DT_PATH_MAX];
    dt_file_t files[DT_MAX_FILES];
    int file_count;
    int file_sel;
    int file_scroll;
    bool files_loading;
    bool files_truncated;

    int dock_sel;

    char status[64];
} desktop_state_t;

void desktop_init_palette(void);
void desktop_state_init(desktop_state_t* s);

/* Full redraw of the whole framebuffer. */
void desktop_render(const desktop_state_t* s);
/* Redraws only the Flipper screen contents (cheap; per streamed frame). */
void desktop_render_flipper(const desktop_state_t* s);

/* Keeps file_scroll so that file_sel is visible. */
void desktop_files_clamp(desktop_state_t* s);

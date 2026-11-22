#include "desktop.h"

#include <stdio.h>
#include <string.h>

#include "assets_gen.h"

/* ---- layout (320x240 logical pixels, shown 2x on 640x480) ---- */

/* 1px frame + 2px dark bezel around the LCD, so it stands apart from the
 * (also orange) focused title bar. */




const dt_dock_item_t desktop_dock_items[] = {
    {"Sub-GHz", &icon_subghz, "Sub-GHz"},
    {"NFC", &icon_nfc, "NFC"},
    {"RFID", &icon_rfid, "125 kHz RFID"},
    {"Infrared", &icon_ir, "Infrared"},
    {"iButton", &icon_ibutton, "iButton"},
    {"Bad KB", &icon_badusb, "Bad KB"},
};
const int desktop_dock_count = sizeof(desktop_dock_items) / sizeof(desktop_dock_items[0]);

void desktop_init_palette(void) {
    static const uint32_t colours[DT_C_COUNT] = {
        [DT_C_BG] = 0x121418,
        [DT_C_BG_DOT] = 0x24282f,
        [DT_C_PANEL] = 0x1d2025,
        [DT_C_PANEL_ALT] = 0x2a2e35,
        [DT_C_BORDER] = 0x40464f,
        [DT_C_SHADOW] = 0x07080a,
        [DT_C_TEXT] = 0xe8e8e8,
        [DT_C_TEXT_DIM] = 0x8d939d,
        [DT_C_ACCENT] = 0xff8200,
        [DT_C_ACCENT_DIM] = 0x7a3f08,
        [DT_C_ON_ACCENT] = 0x000000,
        [DT_C_LCD_BG] = 0xff8200,
        [DT_C_LCD_FG] = 0x000000,
        [DT_C_OK] = 0x45d483,
        [DT_C_WARN] = 0xff5151,
        [DT_C_LCD_OFF] = 0x2b1a0b,
    };
    for(int i = 0; i < DT_C_COUNT; i++) gfx_set_palette((uint8_t)i, colours[i]);
}

void desktop_state_init(desktop_state_t* s) {
    memset(s, 0, sizeof(*s));
    s->battery_pct = -1;
    strcpy(s->cwd, "/ext");
}

void desktop_files_clamp(desktop_state_t* s) {
    if(s->file_sel >= s->file_count) s->file_sel = s->file_count - 1;
    if(s->file_sel < 0) s->file_sel = 0;
    if(s->file_sel < s->file_scroll) s->file_scroll = s->file_sel;
    if(s->file_sel >= s->file_scroll + DT_FILES_VISIBLE)
        s->file_scroll = s->file_sel - DT_FILES_VISIBLE + 1;
    if(s->file_scroll < 0) s->file_scroll = 0;
}

/* ---- widgets ---- */

static void window(int x, int y, int w, int h, const char* title, bool focused) {
    uint8_t edge = focused ? DT_C_ACCENT : DT_C_BORDER;
    gfx_fill_rect(x + 2, y + 2, w, h, DT_C_SHADOW);
    gfx_fill_rect(x, y, w, h, DT_C_PANEL);
    gfx_fill_rect(x, y, w, TITLE_H, edge);
    gfx_rect(x, y, w, h, edge);
    gfx_text_fit(
        x + 4, y + 9, font_primary, title, w - 8, focused ? DT_C_ON_ACCENT : DT_C_TEXT);
}

static void background(void) {
    gfx_clear(DT_C_BG);
    for(int y = TOPBAR_H + 3; y < HINT_Y; y += 8)
        for(int x = 3; x < GFX_W; x += 8) gfx_pixel(x, y, DT_C_BG_DOT);
}

static void topbar(const desktop_state_t* s) {
    gfx_fill_rect(0, 0, GFX_W, TOPBAR_H, DT_C_PANEL);
    gfx_hline(0, TOPBAR_H - 1, GFX_W, DT_C_BORDER);

    int x = 6;
    x += gfx_text(x, 11, font_title, "Flipper", DT_C_ACCENT);
    gfx_text(x + 4, 10, font_secondary, "Desktop", DT_C_TEXT_DIM);

    /* Right side, laid out right to left. */
    x = GFX_W - 6;
    const char* clock = s->clock[0] ? s->clock : "--:--";
    x -= gfx_text_width(font_primary, clock);
    gfx_text(x, 10, font_primary, clock, s->clock[0] ? DT_C_TEXT : DT_C_TEXT_DIM);

    x -= 8 + icon_battery.w;
    gfx_icon(x, 3, &icon_battery, s->battery_pct >= 0 ? DT_C_TEXT : DT_C_TEXT_DIM);
    if(s->battery_pct >= 0) {
        int fill = (s->battery_pct * 20 + 50) / 100;
        uint8_t c = s->battery_pct <= 15 ? DT_C_WARN : DT_C_OK;
        gfx_fill_rect(x + 2, 5, fill, 4, c);
        char pct[6];
        snprintf(pct, sizeof(pct), "%d%%", s->battery_pct);
        int pw = gfx_text_width(font_secondary, pct);
        gfx_text(x - pw - 3, 10, font_secondary, pct, DT_C_TEXT);
        x -= pw + 3;
        if(s->charging) {
            x -= icon_charging.w + 2;
            gfx_icon(x, 2, &icon_charging, DT_C_ACCENT);
        }
    }

    x -= 8 + icon_sd.w;
    gfx_icon(x, 3, &icon_sd, s->sd_present ? DT_C_TEXT : DT_C_BORDER);

    const char* link = s->link_up ? "Linked" : "No link";
    x -= 8 + gfx_text_width(font_secondary, link);
    gfx_text(x, 10, font_secondary, link, s->link_up ? DT_C_OK : DT_C_WARN);
    x -= 4 + icon_expansion.w;
    gfx_icon(x, 3, &icon_expansion, s->link_up ? DT_C_OK : DT_C_WARN);
}

void desktop_render_flipper(const desktop_state_t* s) {
    const int lcd_w = 128 * FLIP_SCALE, lcd_h = 64 * FLIP_SCALE;
    if(s->link_up && s->have_frame) {
        if(s->frame_orientation >= 2) {
            /* Portrait (64x128) only fits the 256x128 LCD at 1x; centre it
             * and blank the sides, which may hold an older landscape frame. */
            const int ox = LCD_X + (lcd_w - 64) / 2;
            gfx_fill_rect(LCD_X, LCD_Y, ox - LCD_X, lcd_h, DT_C_LCD_OFF);
            gfx_fill_rect(ox + 64, LCD_Y, LCD_X + lcd_w - (ox + 64), lcd_h, DT_C_LCD_OFF);
            gfx_flipper_frame_oriented(
                ox, LCD_Y, s->frame, s->frame_orientation, 1, DT_C_LCD_FG, DT_C_LCD_BG);
        } else {
            gfx_flipper_frame_oriented(
                LCD_X,
                LCD_Y,
                s->frame,
                s->frame_orientation,
                FLIP_SCALE,
                DT_C_LCD_FG,
                DT_C_LCD_BG);
        }
        return;
    }
    gfx_fill_rect(LCD_X, LCD_Y, lcd_w, lcd_h, DT_C_LCD_OFF);
    const char* msg = s->link_up ? "Waiting for screen..." : "Waiting for Flipper...";
    int w = gfx_text_width(font_primary, msg);
    gfx_text(LCD_X + (lcd_w - w) / 2, LCD_Y + lcd_h / 2 + 3, font_primary, msg, DT_C_ACCENT);
}

static void flipper_window(const desktop_state_t* s) {
    window(FLIP_X, FLIP_Y, FLIP_W, FLIP_H, "Flipper", s->focus == DT_FOCUS_FLIPPER);
    desktop_render_flipper(s);
}

static void dock(const desktop_state_t* s) {
    bool focused = s->focus == DT_FOCUS_DOCK;
    window(DOCK_X, DOCK_Y, DOCK_W, DOCK_H, "Apps", focused);
    for(int i = 0; i < desktop_dock_count; i++) {
        const dt_dock_item_t* it = &desktop_dock_items[i];
        int tx = DOCK_X + 2, ty = DOCK_Y + TITLE_H + 2 + i * DOCK_TILE_H;
        int tw = DOCK_W - 4, th = DOCK_TILE_H - 1;
        uint8_t fg = DT_C_TEXT;
        if(i == s->dock_sel && focused) {
            gfx_fill_round_rect(tx, ty, tw, th, DT_C_ACCENT);
            fg = DT_C_ON_ACCENT;
        } else if(i == s->dock_sel) {
            gfx_round_rect(tx, ty, tw, th, DT_C_BORDER);
        }
        gfx_icon(tx + (tw - it->icon->w) / 2, ty + 2, it->icon, fg);
        int lw = gfx_text_width(font_tiny, it->label);
        gfx_text(tx + (tw - lw) / 2, ty + th - 2, font_tiny, it->label, fg);
    }
}

static const gfx_icon_t* icon_for(const dt_file_t* f) {
    if(strcmp(f->name, "..") == 0) return &icon_back;
    if(f->is_dir) return &icon_dir;
    const char* dot = strrchr(f->name, '.');
    if(!dot) return &icon_file;
    static const struct {
        const char* ext;
        const gfx_icon_t* icon;
    } map[] = {
        {".sub", &icon_subghz},
        {".nfc", &icon_nfc},
        {".rfid", &icon_rfid},
        {".ir", &icon_ir},
        {".ibtn", &icon_ibutton},
        {".u2f", &icon_u2f},
        {".fap", &icon_apps},
        {".js", &icon_js},
        {".fmf", &icon_music},
        {".txt", &icon_file},
    };
    for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if(strcmp(dot, map[i].ext) == 0) return map[i].icon;
    return &icon_unknown;
}

static void format_size(uint32_t size, char* out, size_t n) {
    if(size < 1024)
        snprintf(out, n, "%luB", (unsigned long)size);
    else if(size < 1024 * 1024)
        snprintf(out, n, "%lu.%luK", (unsigned long)(size / 1024),
                 (unsigned long)(size % 1024 * 10 / 1024));
    else
        snprintf(out, n, "%lu.%luM", (unsigned long)(size >> 20),
                 (unsigned long)((size & 0xFFFFF) * 10 >> 20));
}

static void files_window(const desktop_state_t* s) {
    bool focused = s->focus == DT_FOCUS_FILES;
    char title[DT_PATH_MAX + 8];
    snprintf(title, sizeof(title), "Files  %s", s->cwd);
    window(FILES_X, FILES_Y, FILES_W, FILES_H, title, focused);

    int list_x = FILES_X + 2, list_y = FILES_Y + TITLE_H + 1;
    int list_w = FILES_W - 4 - 6; /* leave room for the scrollbar */
    const char* empty = NULL;
    if(!s->link_up)
        empty = "Connect the Flipper to browse its SD card";
    else if(s->files_loading)
        empty = "Loading...";
    else if(s->file_count == 0)
        empty = "Empty folder";
    if(empty) {
        gfx_text(list_x + 6, list_y + 2 * FILES_ROW_H, font_secondary, empty, DT_C_TEXT_DIM);
        return;
    }

    gfx_set_clip(list_x, list_y, list_w, DT_FILES_VISIBLE * FILES_ROW_H);
    for(int row = 0; row < DT_FILES_VISIBLE; row++) {
        int i = s->file_scroll + row;
        if(i >= s->file_count) break;
        const dt_file_t* f = &s->files[i];
        int y = list_y + row * FILES_ROW_H;
        uint8_t fg = DT_C_TEXT, dim = DT_C_TEXT_DIM;
        if(i == s->file_sel) {
            if(focused) {
                gfx_fill_round_rect(list_x, y, list_w, FILES_ROW_H, DT_C_ACCENT);
                fg = dim = DT_C_ON_ACCENT;
            } else {
                gfx_fill_round_rect(list_x, y, list_w, FILES_ROW_H, DT_C_PANEL_ALT);
            }
        }
        gfx_icon(list_x + 3, y + 1, icon_for(f), fg);
        int name_max = list_w - 18 - 40;
        gfx_text_fit(list_x + 17, y + 9, font_secondary, f->name, name_max, fg);
        if(!f->is_dir) {
            char sz[12];
            format_size(f->size, sz, sizeof(sz));
            int w = gfx_text_width(font_secondary, sz);
            gfx_text(list_x + list_w - 4 - w, y + 9, font_secondary, sz, dim);
        }
    }
    gfx_reset_clip();

    /* Flipper-style scrollbar: dotted track, solid thumb. */
    int sb_x = FILES_X + FILES_W - 5, sb_y = list_y + 1, sb_h = DT_FILES_VISIBLE * FILES_ROW_H - 2;
    if(s->file_count > DT_FILES_VISIBLE) {
        for(int y = sb_y; y < sb_y + sb_h; y += 2) gfx_pixel(sb_x + 1, y, DT_C_BORDER);
        int thumb_h = sb_h * DT_FILES_VISIBLE / s->file_count;
        if(thumb_h < 4) thumb_h = 4;
        int thumb_y = sb_y + (sb_h - thumb_h) * s->file_scroll /
                                 (s->file_count - DT_FILES_VISIBLE);
        gfx_fill_rect(sb_x, thumb_y, 3, thumb_h, focused ? DT_C_ACCENT : DT_C_TEXT_DIM);
    }
}

static void hint_bar(const desktop_state_t* s) {
    gfx_fill_rect(0, HINT_Y, GFX_W, GFX_H - HINT_Y, DT_C_PANEL);
    gfx_hline(0, HINT_Y, GFX_W, DT_C_BORDER);
    const char* hint;
    if(!s->input_ready)
        hint = "No keyboard: plug a USB receiver into the VGM";
    else if(s->focus == DT_FOCUS_FLIPPER)
        hint = "Arrows Enter Esc = Flipper keys   Tab = next window";
    else if(s->focus == DT_FOCUS_FILES)
        hint = "Up/Down select   Enter open   Bksp up   Tab = next";
    else
        hint = "Up/Down select   Enter launch app   Tab = next";
    int right = GFX_W - 6;
    if(s->status[0]) {
        int w = gfx_text_width(font_secondary, s->status);
        gfx_text(right - w, HINT_Y + 9, font_secondary, s->status, DT_C_ACCENT);
        right -= w + 8;
    }
    gfx_text_fit(6, HINT_Y + 9, font_secondary, hint, right - 6, DT_C_TEXT_DIM);
}

void desktop_render(const desktop_state_t* s) {
    gfx_reset_clip();
    background();
    topbar(s);
    flipper_window(s);
    dock(s);
    files_window(s);
    hint_bar(s);
}

#pragma once
/*
 * Minimal 8bpp indexed-colour drawing library for the VGM desktop shell.
 *
 * Portable C with no Pico SDK dependency, so the same code builds on the
 * host (tools/desktop_preview) and renders PNG previews.
 *
 * The framebuffer is 320x240 palette indices. On the VGM, core 1 converts
 * each scanline to RGB565 through gfx_palette while DVI scans out, and the
 * 640x480 output pixel-doubles it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_W 320
#define GFX_H 240

typedef struct {
    uint8_t w;
    uint8_t h;
    const uint8_t* data; /* 1bpp, row-major, MSB first, rows padded to bytes */
} gfx_icon_t;

extern uint8_t gfx_fb[GFX_W * GFX_H];
extern uint16_t gfx_palette[256]; /* RGB565 */

static inline uint16_t gfx_rgb565(uint32_t rgb888) {
    return (uint16_t)(((rgb888 >> 8) & 0xF800) | ((rgb888 >> 5) & 0x07E0) | ((rgb888 >> 3) & 0x001F));
}

void gfx_set_palette(uint8_t index, uint32_t rgb888);

/* Clip rectangle applies to every primitive below. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);

void gfx_clear(uint8_t c);
void gfx_pixel(int x, int y, uint8_t c);
void gfx_hline(int x, int y, int w, uint8_t c);
void gfx_vline(int x, int y, int h, uint8_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t c);
void gfx_rect(int x, int y, int w, int h, uint8_t c);
/* Rectangle with 1px-chamfered corners, Flipper-UI style. */
void gfx_round_rect(int x, int y, int w, int h, uint8_t c);
void gfx_fill_round_rect(int x, int y, int w, int h, uint8_t c);
/* 50% checkerboard fill (Flipper's "disabled"/scrollbar texture). */
void gfx_dither_rect(int x, int y, int w, int h, uint8_t c);

/* Draws set bits of an icon in colour c; clear bits are transparent. */
void gfx_icon(int x, int y, const gfx_icon_t* icon, uint8_t c);

/*
 * Blits a Flipper screen frame (128x64, 8 pages x 128 columns, bit 0 = top
 * row of each page) at integer scale.
 */
void gfx_flipper_frame(int x, int y, const uint8_t* frame, int scale, uint8_t on, uint8_t off);

/*
 * Same, but undoes the Flipper canvas rotation first. The frame is always
 * the raw 128x64 panel buffer; orientation is PB_Gui_ScreenOrientation
 * (0 horizontal, 1 horizontal flip, 2 vertical, 3 vertical flip). Vertical
 * frames come out 64 wide by 128 tall.
 */
void gfx_flipper_frame_oriented(
    int x,
    int y,
    const uint8_t* frame,
    uint8_t orientation,
    int scale,
    uint8_t on,
    uint8_t off);

/*
 * Text in a raw u8g2 font. y is the baseline. Returns the advance width.
 * Only the 0..255 encoding range is supported.
 */
int gfx_text(int x, int y, const uint8_t* font, const char* s, uint8_t c);
int gfx_text_width(const uint8_t* font, const char* s);
int gfx_font_ascent(const uint8_t* font); /* height of 'A' above baseline */
int gfx_font_descent(const uint8_t* font); /* depth of 'g' below baseline, negative */
/* Truncates with "..." so the string fits in max_w pixels. */
int gfx_text_fit(int x, int y, const uint8_t* font, const char* s, int max_w, uint8_t c);

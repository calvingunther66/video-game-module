#include "gfx.h"

#include <string.h>

uint8_t gfx_fb[GFX_W * GFX_H];
uint16_t gfx_palette[256];

static int clip_x0 = 0, clip_y0 = 0, clip_x1 = GFX_W, clip_y1 = GFX_H;

void gfx_set_palette(uint8_t index, uint32_t rgb888) {
    gfx_palette[index] = gfx_rgb565(rgb888);
}

void gfx_set_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x;
    clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > GFX_W ? GFX_W : x + w;
    clip_y1 = y + h > GFX_H ? GFX_H : y + h;
}

void gfx_reset_clip(void) {
    gfx_set_clip(0, 0, GFX_W, GFX_H);
}

void gfx_clear(uint8_t c) {
    memset(gfx_fb, c, sizeof(gfx_fb));
}

void gfx_pixel(int x, int y, uint8_t c) {
    if(x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    gfx_fb[y * GFX_W + x] = c;
}

void gfx_hline(int x, int y, int w, uint8_t c) {
    if(y < clip_y0 || y >= clip_y1) return;
    int x1 = x + w;
    if(x < clip_x0) x = clip_x0;
    if(x1 > clip_x1) x1 = clip_x1;
    if(x1 <= x) return;
    memset(&gfx_fb[y * GFX_W + x], c, (size_t)(x1 - x));
}

void gfx_vline(int x, int y, int h, uint8_t c) {
    if(x < clip_x0 || x >= clip_x1) return;
    int y1 = y + h;
    if(y < clip_y0) y = clip_y0;
    if(y1 > clip_y1) y1 = clip_y1;
    for(; y < y1; y++) gfx_fb[y * GFX_W + x] = c;
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t c) {
    for(int i = 0; i < h; i++) gfx_hline(x, y + i, w, c);
}

void gfx_rect(int x, int y, int w, int h, uint8_t c) {
    if(w <= 0 || h <= 0) return;
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y + 1, h - 2, c);
    gfx_vline(x + w - 1, y + 1, h - 2, c);
}

void gfx_round_rect(int x, int y, int w, int h, uint8_t c) {
    if(w < 3 || h < 3) return gfx_rect(x, y, w, h, c);
    gfx_hline(x + 1, y, w - 2, c);
    gfx_hline(x + 1, y + h - 1, w - 2, c);
    gfx_vline(x, y + 1, h - 2, c);
    gfx_vline(x + w - 1, y + 1, h - 2, c);
}

void gfx_fill_round_rect(int x, int y, int w, int h, uint8_t c) {
    if(w < 3 || h < 3) return gfx_fill_rect(x, y, w, h, c);
    gfx_hline(x + 1, y, w - 2, c);
    gfx_fill_rect(x, y + 1, w, h - 2, c);
    gfx_hline(x + 1, y + h - 1, w - 2, c);
}

void gfx_dither_rect(int x, int y, int w, int h, uint8_t c) {
    for(int j = 0; j < h; j++)
        for(int i = (x + y + j) & 1; i < w; i += 2) gfx_pixel(x + i, y + j, c);
}

void gfx_icon(int x, int y, const gfx_icon_t* icon, uint8_t c) {
    int stride = (icon->w + 7) / 8;
    for(int j = 0; j < icon->h; j++) {
        const uint8_t* row = &icon->data[j * stride];
        for(int i = 0; i < icon->w; i++)
            if(row[i >> 3] & (0x80 >> (i & 7))) gfx_pixel(x + i, y + j, c);
    }
}

void gfx_flipper_frame(int x, int y, const uint8_t* frame, int scale, uint8_t on, uint8_t off) {
    for(int fy = 0; fy < 64; fy++) {
        const uint8_t* page = &frame[(fy >> 3) * 128];
        uint8_t mask = (uint8_t)(1u << (fy & 7));
        for(int sy = 0; sy < scale; sy++) {
            int py = y + fy * scale + sy;
            if(py < clip_y0 || py >= clip_y1) continue;
            uint8_t* dst = &gfx_fb[py * GFX_W];
            for(int fx = 0; fx < 128; fx++) {
                uint8_t c = (page[fx] & mask) ? on : off;
                int px = x + fx * scale;
                for(int sx = 0; sx < scale; sx++, px++)
                    if(px >= clip_x0 && px < clip_x1) dst[px] = c;
            }
        }
    }
}

/* ---- u8g2 font decoding (see lib/u8g2/u8g2_font.c for the format) ---- */

#define U8G2_HDR_SIZE 23

typedef struct {
    const uint8_t* ptr;
    uint8_t bit_pos;
} bitreader_t;

static unsigned get_bits(bitreader_t* r, uint8_t cnt) {
    unsigned val = (unsigned)(*r->ptr) >> r->bit_pos;
    uint8_t end = r->bit_pos + cnt;
    if(end >= 8) {
        r->ptr++;
        val |= (unsigned)(*r->ptr) << (8 - r->bit_pos);
        end -= 8;
    }
    r->bit_pos = end;
    return val & ((1u << cnt) - 1);
}

static int get_signed_bits(bitreader_t* r, uint8_t cnt) {
    return (int)get_bits(r, cnt) - (1 << (cnt - 1));
}

static const uint8_t* find_glyph(const uint8_t* font, uint8_t enc) {
    const uint8_t* p = font + U8G2_HDR_SIZE;
    if(enc >= 'a')
        p += (font[19] << 8) | font[20];
    else if(enc >= 'A')
        p += (font[17] << 8) | font[18];
    for(;;) {
        if(p[1] == 0) return NULL;
        if(p[0] == enc) return p + 2;
        p += p[1];
    }
}

/* Decodes (and optionally draws) one glyph; returns its x advance. */
static int glyph(const uint8_t* font, uint8_t enc, int x, int y, bool draw, uint8_t c) {
    const uint8_t* g = find_glyph(font, enc);
    if(!g) return 0;
    bitreader_t r = {g, 0};
    unsigned w = get_bits(&r, font[4]);
    unsigned h = get_bits(&r, font[5]);
    int gx = get_signed_bits(&r, font[6]);
    int gy = get_signed_bits(&r, font[7]);
    int dx = get_signed_bits(&r, font[8]);
    if(!draw || w == 0) return dx;

    int ox = x + gx;
    int oy = y - (int)h - gy;
    unsigned lx = 0, ly = 0;
    while(ly < h) {
        unsigned a = get_bits(&r, font[2]);
        unsigned b = get_bits(&r, font[3]);
        do {
            for(int pass = 0; pass < 2; pass++) {
                unsigned len = pass ? b : a;
                while(len) {
                    unsigned run = w - lx;
                    if(len < run) run = len;
                    if(pass) gfx_hline(ox + (int)lx, oy + (int)ly, (int)run, c);
                    len -= run;
                    lx += run;
                    if(lx >= w) {
                        lx = 0;
                        ly++;
                    }
                }
            }
        } while(get_bits(&r, 1) != 0);
    }
    return dx;
}

int gfx_text(int x, int y, const uint8_t* font, const char* s, uint8_t c) {
    int x0 = x;
    for(; *s; s++) x += glyph(font, (uint8_t)*s, x, y, true, c);
    return x - x0;
}

int gfx_text_width(const uint8_t* font, const char* s) {
    int w = 0;
    for(; *s; s++) w += glyph(font, (uint8_t)*s, 0, 0, false, 0);
    return w;
}

int gfx_font_ascent(const uint8_t* font) {
    return (int8_t)font[13];
}

int gfx_font_descent(const uint8_t* font) {
    return (int8_t)font[14];
}

int gfx_text_fit(int x, int y, const uint8_t* font, const char* s, int max_w, uint8_t c) {
    if(gfx_text_width(font, s) <= max_w) return gfx_text(x, y, font, s, c);
    int dots = gfx_text_width(font, "...");
    int w = 0;
    int x0 = x;
    for(; *s; s++) {
        int d = glyph(font, (uint8_t)*s, 0, 0, false, 0);
        if(w + d + dots > max_w) break;
        x += glyph(font, (uint8_t)*s, x, y, true, c);
        w += d;
    }
    x += gfx_text(x, y, font, "...", c);
    return x - x0;
}

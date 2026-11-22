/*
 * Host-side preview of the VGM desktop shell.
 *
 * Builds app/desktop/{gfx,desktop,assets_gen}.c natively, renders a few
 * scenes and writes them as 640x480 PNGs, exactly as the VGM would send
 * them over HDMI (RGB565 palette, each logical pixel doubled).
 *
 *   make -C tools/desktop_preview && ls tools/desktop_preview/out
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets_gen.h"
#include "desktop.h"

/* ---- minimal PNG writer (stored deflate, no zlib dependency) ---- */

static uint32_t crc_table[256];

static void crc_init(void) {
    for(uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for(int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    c ^= 0xFFFFFFFFu;
    while(n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if(len) fwrite(data, 1, len, f);
    uint32_t c = crc(0, (const uint8_t*)type, 4);
    c = crc(c, data, len);
    uint8_t cb[4];
    put32(cb, c);
    fwrite(cb, 1, 4, f);
}

static void write_png(const char* path, const uint8_t* rgb, int w, int h) {
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t* raw = malloc(raw_len);
    for(int y = 0; y < h; y++) {
        raw[y * (1 + w * 3)] = 0;
        memcpy(&raw[y * (1 + w * 3) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3);
    }
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t* z = malloc(z_len);
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for(size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for(size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[o++] = off + n >= raw_len;
        z[o++] = n & 0xFF;
        z[o++] = n >> 8;
        z[o++] = ~n & 0xFF;
        z[o++] = (~n >> 8) & 0xFF;
        memcpy(&z[o], &raw[off], n);
        o += n;
    }
    put32(&z[o], (b << 16) | a);
    o += 4;

    FILE* f = fopen(path, "wb");
    if(!f) {
        perror(path);
        exit(1);
    }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {0};
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 2; /* RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    printf("wrote %s\n", path);
}

/* Converts the framebuffer exactly like core 1 does, then doubles it. */
static void snapshot(const char* path) {
    const int W = GFX_W * 2, H = GFX_H * 2;
    uint8_t* rgb = malloc((size_t)W * H * 3);
    for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++) {
            uint16_t c = gfx_palette[gfx_fb[(y / 2) * GFX_W + x / 2]];
            uint8_t* p = &rgb[((size_t)y * W + x) * 3];
            p[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
            p[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
            p[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        }
    write_png(path, rgb, W, H);
    free(rgb);
}

static void load_frame(const char* path, uint8_t* out) {
    FILE* f = fopen(path, "rb");
    if(!f || fread(out, 1, 1024, f) != 1024) {
        fprintf(stderr, "cannot read 1024 B frame from %s\n", path);
        exit(1);
    }
    fclose(f);
}

static void add_file(desktop_state_t* s, const char* name, bool dir, uint32_t size) {
    dt_file_t* f = &s->files[s->file_count++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->is_dir = dir;
    f->size = size;
}

static void font_sheet(const char* path) {
    static const struct {
        const char* name;
        const uint8_t* font;
    } fonts[] = {
        {"primary helvB08", font_primary},
        {"secondary haxrcorp4089", font_secondary},
        {"mono profont11", font_mono},
        {"title helvB10", font_title},
        {"tiny 4x6", font_tiny},
    };
    gfx_reset_clip();
    gfx_clear(DT_C_PANEL);
    int y = 14;
    for(size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        gfx_text(4, y, font_tiny, fonts[i].name, DT_C_TEXT_DIM);
        y += gfx_font_ascent(fonts[i].font) + 3;
        gfx_text(4, y, fonts[i].font, "The quick brown fox jumps over 0123456789", DT_C_TEXT);
        y += 2 - gfx_font_descent(fonts[i].font) + 2;
        gfx_text(4, y + gfx_font_ascent(fonts[i].font), fonts[i].font,
                 "ABCDEFGHIJKLM nopqrstuvwxyz !?.,:;()[]/_-+=%", DT_C_ACCENT);
        y += gfx_font_ascent(fonts[i].font) + 10;
    }
    gfx_text(4, y + 18, font_big_numbers, "12:34", DT_C_TEXT);
    const gfx_icon_t* icons[] = {&icon_dir, &icon_file, &icon_unknown, &icon_subghz, &icon_nfc,
                                 &icon_rfid, &icon_ir, &icon_ibutton, &icon_badusb, &icon_u2f,
                                 &icon_apps, &icon_js, &icon_music, &icon_settings, &icon_back,
                                 &icon_keyboard, &icon_search, &icon_sd, &icon_battery,
                                 &icon_charging, &icon_expansion};
    int x = 90;
    for(size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        if(x + icons[i]->w > GFX_W - 4) {
            x = 90;
            y += 14;
        }
        gfx_icon(x, y + 6, icons[i], DT_C_TEXT);
        x += icons[i]->w + 4;
    }
    snapshot(path);
}

int main(int argc, char** argv) {
    const char* frame_path = argc > 1 ? argv[1] : "frames/desktop.bin";
    const char* out = argc > 2 ? argv[2] : "out";
    char path[256];
    crc_init();
    desktop_init_palette();

    static desktop_state_t s;
    desktop_state_init(&s);
    s.link_up = true;
    s.have_frame = true;
    load_frame(frame_path, s.frame);
    s.battery_pct = 76;
    s.sd_present = true;
    strcpy(s.clock, "14:32");
    s.input_ready = true;
    strcpy(s.cwd, "/ext/subghz");
    add_file(&s, "..", true, 0);
    add_file(&s, "assets", true, 0);
    add_file(&s, "Garage_Door_433.sub", false, 2381);
    add_file(&s, "Tesla_Charge_Port_EU_and_AU_Long_Name.sub", false, 18533);
    add_file(&s, "Weather_Station.sub", false, 912);
    add_file(&s, "doorbell.sub", false, 1290444);
    add_file(&s, "notes.txt", false, 47);

    s.focus = DT_FOCUS_FLIPPER;
    desktop_render(&s);
    snprintf(path, sizeof(path), "%s/01_flipper_focus.png", out);
    snapshot(path);

    s.focus = DT_FOCUS_FILES;
    s.file_sel = 3;
    desktop_files_clamp(&s);
    strcpy(s.status, "Copied 2 files");
    desktop_render(&s);
    snprintf(path, sizeof(path), "%s/02_files_focus.png", out);
    snapshot(path);

    s.focus = DT_FOCUS_DOCK;
    s.dock_sel = 2;
    s.status[0] = 0;
    s.charging = true;
    s.battery_pct = 9;
    desktop_render(&s);
    snprintf(path, sizeof(path), "%s/03_dock_focus.png", out);
    snapshot(path);

    desktop_state_init(&s);
    desktop_render(&s);
    snprintf(path, sizeof(path), "%s/04_no_link.png", out);
    snapshot(path);

    snprintf(path, sizeof(path), "%s/05_font_sheet.png", out);
    font_sheet(path);
    return 0;
}

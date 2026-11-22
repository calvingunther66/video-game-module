/*
 * USB display stream: see usb_display.h for the wire format.
 *
 * Runs inside the TinyUSB task (this firmware has no TinyUSB OS locking, so
 * every tud_cdc_* call stays on that one task). Every SCAN_INTERVAL_MS it
 * walks the framebuffer, hashes each row and sends only rows whose hash
 * changed, RLE-packed (the desktop is mostly flat colour, so a row is
 * usually a few dozen bytes). Rows are copied before hashing/encoding, so a
 * row core 0 is drawing at that moment is at worst sent once more next scan.
 */
#include "usb_display.h"

#include <FreeRTOS.h>
#include <task.h>
#include <string.h>

#include "tusb.h"

#include "desktop.h"
#include "desktop_task.h"
#include "gfx.h"

#define SCAN_INTERVAL_MS 33
#define ROWS_PER_CALL 8

#define MAGIC0 0xA5
#define MAGIC1 0x5A
#define HDR_LEN 5

#define MSG_HELLO 0x01
#define MSG_ROW 0x02
#define MSG_FRAME 0x03
#define MSG_REFRESH 0x10
#define MSG_KEY 0x11
#define MSG_CLICK 0x12
#define MSG_WHEEL 0x13

/* Worst case ROW: header + y + 2 bytes per pixel. */
static uint8_t out[HDR_LEN + 2 + 2 * GFX_W];
static uint16_t out_len, out_pos;

static uint32_t row_hash[GFX_H];
static bool row_valid[GFX_H];
static uint8_t row_copy[GFX_W];

static bool connected;
static bool want_hello;
static int scan_y = -1; /* -1: idle, waiting for the next scan */
static TickType_t last_scan;
static uint16_t frame_seq;

static uint8_t rx[64];
static uint16_t rx_len;

static void begin(uint8_t type) {
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = type;
    out_len = HDR_LEN;
    out_pos = 0;
}

static void put16(uint16_t v) {
    out[out_len++] = v & 0xFF;
    out[out_len++] = v >> 8;
}

static void finish(void) {
    uint16_t len = out_len - HDR_LEN;
    out[3] = len & 0xFF;
    out[4] = len >> 8;
}

static void build_hello(void) {
    begin(MSG_HELLO);
    put16(GFX_W);
    put16(GFX_H);
    put16(DT_C_COUNT);
    for(int i = 0; i < DT_C_COUNT; i++) {
        uint16_t c = gfx_palette[i];
        uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        out[out_len++] = (r << 3) | (r >> 2);
        out[out_len++] = (g << 2) | (g >> 4);
        out[out_len++] = (b << 3) | (b >> 2);
    }
    finish();
}

static uint32_t hash_row(const uint8_t* p) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for(int i = 0; i < GFX_W; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void build_row(int y) {
    begin(MSG_ROW);
    put16((uint16_t)y);
    int x = 0;
    while(x < GFX_W) {
        uint8_t v = row_copy[x];
        int run = 1;
        while(x + run < GFX_W && run < 255 && row_copy[x + run] == v) run++;
        out[out_len++] = (uint8_t)run;
        out[out_len++] = v;
        x += run;
    }
    finish();
}

/* Writes what fits of the pending message; true when it is fully sent. */
static bool drain(void) {
    while(out_pos < out_len) {
        uint32_t avail = tud_cdc_n_write_available(USB_DISPLAY_CDC);
        if(avail == 0) {
            tud_cdc_n_write_flush(USB_DISPLAY_CDC);
            return false;
        }
        uint32_t n = out_len - out_pos;
        if(n > avail) n = avail;
        out_pos += tud_cdc_n_write(USB_DISPLAY_CDC, &out[out_pos], n);
    }
    out_len = out_pos = 0;
    return true;
}

static void handle_message(uint8_t type, const uint8_t* p, uint16_t len) {
    desktop_input_t in = {0};
    switch(type) {
    case MSG_REFRESH:
        want_hello = true;
        memset(row_valid, 0, sizeof(row_valid));
        return;
    case MSG_KEY:
        if(len < 2) return;
        in.type = DESKTOP_INPUT_KEY;
        in.key = p[0];
        in.long_press = p[1] & 1;
        break;
    case MSG_CLICK:
        if(len < 5) return;
        in.type = DESKTOP_INPUT_CLICK;
        in.x = p[0] | (p[1] << 8);
        in.y = p[2] | (p[3] << 8);
        break;
    case MSG_WHEEL:
        if(len < 1) return;
        in.type = DESKTOP_INPUT_WHEEL;
        in.delta = (int8_t)p[0];
        break;
    default:
        return;
    }
    desktop_task_post_input(&in);
}

static void poll_rx(void) {
    while(tud_cdc_n_available(USB_DISPLAY_CDC)) {
        uint8_t b;
        if(tud_cdc_n_read(USB_DISPLAY_CDC, &b, 1) != 1) break;
        /* Resync on the magic bytes. */
        if(rx_len == 0 && b != MAGIC0) continue;
        if(rx_len == 1 && b != MAGIC1) {
            rx_len = (b == MAGIC0) ? 1 : 0;
            continue;
        }
        rx[rx_len++] = b;
        if(rx_len >= HDR_LEN) {
            uint16_t len = rx[3] | (rx[4] << 8);
            if(len > sizeof(rx) - HDR_LEN) {
                rx_len = 0;
                continue;
            }
            if(rx_len == HDR_LEN + len) {
                handle_message(rx[2], &rx[HDR_LEN], len);
                rx_len = 0;
            }
        }
    }
}

void usb_display_service(void) {
    bool now_connected = tud_cdc_n_connected(USB_DISPLAY_CDC);
    if(now_connected != connected) {
        connected = now_connected;
        out_len = out_pos = 0;
        rx_len = 0;
        scan_y = -1;
        if(connected) {
            want_hello = true;
            memset(row_valid, 0, sizeof(row_valid));
        }
        desktop_task_set_remote(connected);
    }
    if(!connected) return;

    poll_rx();

    if(!drain()) return;

    if(want_hello) {
        want_hello = false;
        build_hello();
        if(!drain()) return;
    }

    const TickType_t now = xTaskGetTickCount();
    if(scan_y < 0) {
        if(now - last_scan < pdMS_TO_TICKS(SCAN_INTERVAL_MS)) return;
        last_scan = now;
        scan_y = 0;
    }

    for(int budget = ROWS_PER_CALL; budget > 0 && scan_y < GFX_H; budget--) {
        const int y = scan_y++;
        memcpy(row_copy, &gfx_fb[y * GFX_W], GFX_W);
        uint32_t h = hash_row(row_copy);
        if(row_valid[y] && row_hash[y] == h) continue;
        row_hash[y] = h;
        row_valid[y] = true;
        build_row(y);
        if(!drain()) return; /* finish it on the next call */
    }

    if(scan_y >= GFX_H) {
        scan_y = -1;
        begin(MSG_FRAME);
        put16(frame_seq++);
        finish();
        drain();
        tud_cdc_n_write_flush(USB_DISPLAY_CDC);
    }
}

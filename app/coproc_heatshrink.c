#include "coproc_heatshrink.h"

#include <string.h>

#include "heatshrink/heatshrink_decoder.h"
#include "heatshrink/heatshrink_encoder.h"

/* Must match COMPRESS_* in Momentum lib/toolbox/compress.c. */
#define HS_WINDOW_BITS 8
#define HS_LOOKAHEAD_BITS 4
#define HS_DECODER_INPUT_BUFFER 256
#define HS_HEADER_SIZE 4

bool coproc_heatshrink_encode(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size) {
    *out_size = 0;
    if(!in_size || out_cap < HS_HEADER_SIZE) return false;

    heatshrink_encoder* enc = heatshrink_encoder_alloc(HS_WINDOW_BITS, HS_LOOKAHEAD_BITS);
    if(!enc) return false;

    size_t pos = HS_HEADER_SIZE;
    size_t sunk = 0;
    bool failed = false;
    while(sunk < in_size && !failed) {
        size_t n = 0;
        if(heatshrink_encoder_sink(enc, (uint8_t*)&in[sunk], in_size - sunk, &n) != HSER_SINK_OK) {
            failed = true;
            break;
        }
        sunk += n;
        HSE_poll_res pr;
        do {
            pr = heatshrink_encoder_poll(enc, &out[pos], out_cap - pos, &n);
            if(pr < 0) failed = true;
            pos += n;
        } while(!failed && pr == HSER_POLL_MORE && pos < out_cap);
        if(pr == HSER_POLL_MORE) failed = true; /* out of room */
    }
    if(!failed) {
        HSE_finish_res fr = heatshrink_encoder_finish(enc);
        while(!failed && fr == HSER_FINISH_MORE) {
            size_t n = 0;
            HSE_poll_res pr = heatshrink_encoder_poll(enc, &out[pos], out_cap - pos, &n);
            if(pr < 0 || (n == 0 && pos >= out_cap)) failed = true;
            pos += n;
            fr = heatshrink_encoder_finish(enc);
        }
        if(fr < 0) failed = true;
    }
    heatshrink_encoder_free(enc);

    const size_t stream = pos - HS_HEADER_SIZE;
    if(!failed && pos < in_size + 1 && stream <= UINT16_MAX) {
        out[0] = 0x01;
        out[1] = 0x00;
        out[2] = (uint8_t)(stream & 0xff);
        out[3] = (uint8_t)(stream >> 8);
        *out_size = pos;
        return true;
    }
    /* Not worth it (or didn't fit): store raw, like compress_encode_internal. */
    if(out_cap > in_size) {
        out[0] = 0x00;
        memcpy(&out[1], in, in_size);
        *out_size = in_size + 1;
        return true;
    }
    return false;
}

bool coproc_heatshrink_decode(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size) {
    *out_size = 0;
    if(in_size < 1) return false;

    if(!in[0]) {
        if(in_size - 1 > out_cap) return false;
        memcpy(out, &in[1], in_size - 1);
        *out_size = in_size - 1;
        return true;
    }

    if(in_size < HS_HEADER_SIZE) return false;
    const size_t stream = (size_t)in[2] | ((size_t)in[3] << 8);
    if(stream > in_size - HS_HEADER_SIZE) return false;
    const uint8_t* src = &in[HS_HEADER_SIZE];

    heatshrink_decoder* dec =
        heatshrink_decoder_alloc(HS_DECODER_INPUT_BUFFER, HS_WINDOW_BITS, HS_LOOKAHEAD_BITS);
    if(!dec) return false;

    size_t sunk = 0, pos = 0;
    bool failed = false;
    while(!failed) {
        if(sunk < stream) {
            size_t n = 0;
            if(heatshrink_decoder_sink(dec, (uint8_t*)&src[sunk], stream - sunk, &n) < 0) {
                failed = true;
                break;
            }
            sunk += n;
        }
        HSD_poll_res pr;
        do {
            size_t n = 0;
            pr = heatshrink_decoder_poll(dec, &out[pos], out_cap - pos, &n);
            if(pr < 0) failed = true;
            pos += n;
            if(pr == HSDR_POLL_MORE && pos >= out_cap) failed = true; /* too big */
        } while(!failed && pr == HSDR_POLL_MORE);
        if(failed) break;
        if(sunk >= stream) {
            HSD_finish_res fr = heatshrink_decoder_finish(dec);
            if(fr == HSDR_FINISH_DONE) break;
            if(fr < 0) failed = true;
        }
    }
    heatshrink_decoder_free(dec);
    if(failed) return false;
    *out_size = pos;
    return true;
}

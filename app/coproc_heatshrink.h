#pragma once
/*
 * Heatshrink compress/decompress for the coproc COMPRESS_/DECOMPRESS_
 * HEATSHRINK tasks. Byte-compatible with Momentum's lib/toolbox/compress.c
 * (compress_encode/compress_decode with compress_config_heatshrink_default):
 * window 2^8, lookahead 2^4, and the same 4-byte header:
 *   [u8 is_compressed][u8 reserved][u16le compressed_size] then the stream,
 * or, when compression doesn't help, [u8 0x00] followed by the raw input.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool coproc_heatshrink_encode(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size);

bool coproc_heatshrink_decode(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size);

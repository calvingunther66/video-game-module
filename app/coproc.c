#include "coproc.h"
#include "usb_hid.h"
#include "coproc_heatshrink.h"

#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

typedef struct {
    uint16_t job_id;
    uint8_t task_type;
    size_t data_size;
} CoprocJobMsg;

typedef struct {
    uint16_t job_id;
    uint8_t task_type;
    bool is_error;
    size_t data_size;
} CoprocResultMsg;

static uint8_t rx_buffer[COPROC_MAX_MESSAGE_SIZE];
static uint8_t tx_buffer[COPROC_MAX_MESSAGE_SIZE];

static QueueHandle_t job_queue;
static QueueHandle_t result_queue;

uint8_t* coproc_get_rx_buffer(void) {
    return rx_buffer;
}

uint8_t* coproc_get_tx_buffer(void) {
    return tx_buffer;
}

void coproc_submit_job(uint16_t job_id, uint8_t task_type, size_t data_size) {
    const CoprocJobMsg msg = {
        .job_id = job_id,
        .task_type = task_type,
        .data_size = data_size,
    };
    // Non-blocking - see the doc comment in coproc.h.
    xQueueSend(job_queue, &msg, 0);
}

bool coproc_result_ready(void) {
    return result_queue && uxQueueMessagesWaiting(result_queue) > 0;
}

bool coproc_poll_result(uint16_t* job_id, uint8_t* task_type, bool* is_error, size_t* data_size) {
    CoprocResultMsg msg;
    if(xQueueReceive(result_queue, &msg, 0) != pdTRUE) return false;

    *job_id = msg.job_id;
    *task_type = msg.task_type;
    *is_error = msg.is_error;
    *data_size = msg.data_size;
    return true;
}

// Same reflected nibble-table CRC-32/ISO-HDLC algorithm as Momentum's
// lib/toolbox/crc32_calc.c (crc32_calc_buffer), so results match bit-for-bit
// with the Flipper-side local fallback.
static uint32_t coproc_crc32(const uint8_t* data, size_t size) {
    static const uint32_t rtable[16] = {
        0x00000000,
        0x1db71064,
        0x3b6e20c8,
        0x26d930ac,
        0x76dc4190,
        0x6b6b51f4,
        0x4db26158,
        0x5005713c,
        0xedb88320,
        0xf00f9344,
        0xd6d6a3e8,
        0xcb61b38c,
        0x9b64c2b0,
        0x86d3d2d4,
        0xa00ae278,
        0xbdbdf21c,
    };

    uint32_t crc = ~0u;
    for(size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }
    return ~crc;
}

// HID-inject command codec. Wire format is shared with the Flipper-side
// Phase 1 BLE "agent_hid" protocol (see coproc.h's COPROC_TASK_HID_INJECT
// comment) - keep in sync with that codec, not invented independently here.
// Any command type outside the subset below (e.g. a future CMD_TYPE_TEXT or
// CMD_CONSUMER_KEY) is skipped over via its own length prefix rather than
// treated as an error, so a job built from that same generic command stream
// still decodes cleanly on this side.
typedef enum {
    CoprocHidCmdKeyPress = 0x01, // payload: u16 LE keycode
    CoprocHidCmdKeyRelease = 0x02, // payload: u16 LE keycode
    CoprocHidCmdMouseMove = 0x04, // payload: i8 dx, i8 dy
    CoprocHidCmdMouseClick = 0x05, // payload: u8 button, u8 down
    CoprocHidCmdMouseScroll = 0x06, // payload: i8 delta
    CoprocHidCmdReleaseAll = 0x08, // no payload
} CoprocHidCmdType;

static inline uint16_t coproc_read_u16_le(const uint8_t* p) {
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

// Decodes and executes every command message packed into a
// COPROC_TASK_HID_INJECT job payload: [u8 type][u16 len LE][payload bytes],
// repeated back-to-back. Returns false if the payload itself is malformed
// (a length prefix runs past the end of the job) or if any individual
// command's execution (usb_hid.c) failed; a bad/mismatched-length payload
// for a *known* command type is treated as that one command failing, not as
// a framing error, so the rest of the job still gets a chance to run.
static bool coproc_hid_inject(const uint8_t* data, size_t data_size) {
    bool all_ok = true;
    size_t offset = 0;

    while(offset < data_size) {
        if(data_size - offset < 3) return false; // truncated type+len header

        const uint8_t type = data[offset];
        const uint16_t len = coproc_read_u16_le(data + offset + 1);
        offset += 3;

        if(len > data_size - offset) return false; // length runs past the job
        const uint8_t* payload = data + offset;
        offset += len;

        switch(type) {
        case CoprocHidCmdKeyPress:
            if(len != 2 || !usb_hid_key_press(coproc_read_u16_le(payload))) all_ok = false;
            break;
        case CoprocHidCmdKeyRelease:
            if(len != 2 || !usb_hid_key_release(coproc_read_u16_le(payload))) all_ok = false;
            break;
        case CoprocHidCmdMouseMove:
            if(len != 2 || !usb_hid_mouse_move((int8_t)payload[0], (int8_t)payload[1]))
                all_ok = false;
            break;
        case CoprocHidCmdMouseClick:
            if(len != 2 || !usb_hid_mouse_click(payload[0], payload[1] != 0)) all_ok = false;
            break;
        case CoprocHidCmdMouseScroll:
            if(len != 1 || !usb_hid_mouse_scroll((int8_t)payload[0])) all_ok = false;
            break;
        case CoprocHidCmdReleaseAll:
            if(len != 0 || !usb_hid_release_all()) all_ok = false;
            break;
        default:
            break; // not ours - already skipped via the length prefix above
        }
    }

    return all_ok;
}

static void compute_task(void* unused_arg) {
    (void)unused_arg;

    for(;;) {
        CoprocJobMsg job;
        if(xQueueReceive(job_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        CoprocResultMsg result = {
            .job_id = job.job_id,
            .task_type = job.task_type,
            .is_error = true,
            .data_size = 0,
        };

        switch(job.task_type) {
        case COPROC_TASK_CRC32: {
            const uint32_t crc = coproc_crc32(rx_buffer, job.data_size);
            memcpy(tx_buffer, &crc, sizeof(crc));
            result.data_size = sizeof(crc);
            result.is_error = false;
            break;
        }
        case COPROC_TASK_COMPRESS_HEATSHRINK:
        case COPROC_TASK_DECOMPRESS_HEATSHRINK: {
            // Byte-compatible with Momentum's compress_encode/decode (see
            // coproc_heatshrink.h). Any failure, including a result larger
            // than the TX buffer, is reported as an error so the Flipper
            // falls back to its own implementation.
            size_t out_size = 0;
            const bool ok = job.task_type == COPROC_TASK_COMPRESS_HEATSHRINK ?
                                coproc_heatshrink_encode(
                                    rx_buffer, job.data_size, tx_buffer, COPROC_MAX_MESSAGE_SIZE, &out_size) :
                                coproc_heatshrink_decode(
                                    rx_buffer, job.data_size, tx_buffer, COPROC_MAX_MESSAGE_SIZE, &out_size);
            if(ok) {
                result.data_size = out_size;
                result.is_error = false;
            }
            break;
        }
        case COPROC_TASK_HID_INJECT: {
            // Fire-and-forget actuation, not a compute-in/result-out task:
            // the output is a single status byte, not a computed result.
            // is_error stays false here even when a command failed - that
            // failure is reported via the status byte instead, since (unlike
            // COMPRESS/DECOMPRESS_HEATSHRINK above) there is no local
            // fallback for the Flipper side to fall back to on this task
            // type, so an is_error result would just be a dead end there.
            const bool ok = coproc_hid_inject(rx_buffer, job.data_size);
            tx_buffer[0] = ok ? 0 : 1;
            result.data_size = 1;
            result.is_error = false;
            break;
        }
        default:
            break;
        }

        xQueueSend(result_queue, &result, portMAX_DELAY);
    }
}

void coproc_init(void) {
    job_queue = xQueueCreate(1, sizeof(CoprocJobMsg));
    result_queue = xQueueCreate(1, sizeof(CoprocResultMsg));

    // Stack depth is in words (StackType_t = uint32_t on this port), not
    // bytes. Was 2*1024 = 8192 bytes - wildly oversized for a task whose
    // only work is a tight CRC32 loop over a fixed-size buffer with no
    // meaningful call depth or large locals. That 8KB is allocated from the
    // same heap (heap_3, a thin malloc/free wrapper) that
    // expansion_start_virtual_display()'s unchecked first_frame allocation
    // draws from, and was the likely cause of that allocation failing right
    // when the first real display frame was built. 512 words now that the
    // heatshrink jobs run here too; their state (~1.5 KB encoder, ~0.5 KB
    // decoder) is malloc'd per job and freed straight after.
    TaskHandle_t handle = NULL;
    BaseType_t status =
        xTaskCreate(compute_task, "compute_task", 512, NULL, tskIDLE_PRIORITY + 1, &handle);
    configASSERT(status == pdPASS);
    (void)status;
}

#include "coproc.h"

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
        case COPROC_TASK_DECOMPRESS_HEATSHRINK:
            // Not ported to this firmware yet - reported as an error, which
            // makes the Flipper side fall back to its own local
            // implementation automatically. See coproc.h in Momentum's
            // applications/services/coproc/ for the fallback contract.
            break;
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
    // when the first real display frame was built.
    TaskHandle_t handle = NULL;
    BaseType_t status =
        xTaskCreate(compute_task, "compute_task", 256, NULL, tskIDLE_PRIORITY + 1, &handle);
    configASSERT(status == pdPASS);
    (void)status;
}

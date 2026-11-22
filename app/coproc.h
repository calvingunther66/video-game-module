#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Task type ids - must stay in sync with CoprocTaskType in Momentum's
// applications/services/coproc/coproc.h (values are sent over the wire as
// ExpansionFrameCoproc::task_type, so the two enums must match numerically,
// not just by name).
#define COPROC_TASK_CRC32                 0
#define COPROC_TASK_COMPRESS_HEATSHRINK   1
#define COPROC_TASK_DECOMPRESS_HEATSHRINK 2
// Fire-and-forget physical HID actuation (keyboard/mouse), unlike the
// compute-in/result-out tasks above. Job payload is one or more concatenated
// command messages: [u8 type][u16 len LE][payload bytes], repeated until the
// job payload is exhausted - same wire format as the Phase 1 BLE
// "agent_hid" command codec on the Flipper side, so it must be decoded
// identically here (see coproc.c). Output is a single status byte (0 =
// every command in the job executed successfully, 1 = at least one did
// not) - there is no computed result to return, and (unlike
// COMPRESS/DECOMPRESS_HEATSHRINK above) no local fallback is possible on
// the Flipper side either: see the CoprocTaskHidInject comment in
// Momentum's applications/services/coproc/coproc.h.
#define COPROC_TASK_HID_INJECT 3

// Must match EXPANSION_WORKER_COPROC_MAX_MESSAGE_SIZE on the Flipper side.
#define COPROC_MAX_MESSAGE_SIZE 4096

void coproc_init(void);

// Buffer that uart.c reassembles an incoming job request's payload into,
// chunk by chunk, before calling coproc_submit_job().
uint8_t* coproc_get_rx_buffer(void);

// Buffer holding a finished job's result payload once coproc_poll_result()
// has returned true.
uint8_t* coproc_get_tx_buffer(void);

// Called by uart.c once a full job request has been reassembled into the RX
// buffer (data_size bytes). Non-blocking: since only one job is ever in
// flight on this link at a time, a still-busy compute_task causes this job
// to be silently dropped rather than blocking uart_task - the Flipper side
// simply times out and falls back to its own local implementation.
void coproc_submit_job(uint16_t job_id, uint8_t task_type, size_t data_size);

// Non-blocking poll for a finished job, called from uart.c's idle points
// (see expansion_receive_frame_rpc_mode() in uart.c). On true, the result
// payload is in the buffer returned by coproc_get_tx_buffer().
bool coproc_poll_result(uint16_t* job_id, uint8_t* task_type, bool* is_error, size_t* data_size);

// True when coproc_poll_result() would return a result; does not consume it.
bool coproc_result_ready(void);

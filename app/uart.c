#include "uart.h"
#include <pico/stdlib.h>

#include <hardware/gpio.h>
#include <FreeRTOS.h>
#include <task.h>
#include <stream_buffer.h>
#include <queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pb_common.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <flipperzero-protobuf/flipper.pb.h>

#include "frame.h"
#include "led_state.h"
#include "bitmaps.h"
#include "expansion_protocol.h"
#include "coproc.h"

#define UART_ID uart0
#define UART_IRQ UART0_IRQ
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_INIT_BAUD_RATE (9600UL)
// Was 1843200 (raised from 921600 upstream in commit c44ad25, Feb 2024). On
// this specific Flipper+VGM pair, 1843200 reliably produces UART RX
// overrun/framing errors on the Flipper's STM32 side within ~11ms of RPC
// session start - confirmed via a byte-for-byte pristine-upstream build,
// across two different host GCC versions, so it is not specific to any
// application code. The "Official" firmware bundled in the community
// flasher app predates that commit and still uses 921600, which is why it
// alone appeared to work throughout this investigation. Reverted to the
// last confirmed-stable rate; this halves the link's raw throughput, which
// only matters for screen mirroring (the coproc channel's 66-byte frames
// are unaffected). May be liftable again on hardware/cabling with tighter
// timing margin than this unit - not re-attempted here.
//
// Raised again to 2000000 together with the FlipDev Momentum firmware's
// expansion-worker change that moves the Flipper's USART1 RX from one
// interrupt per byte to circular DMA with a deeper staging buffer (the
// suspected cause of the overruns above). 2 Mbaud rather than 1843200 because
// it divides exactly on both ends: STM32 64 MHz / 16 / 32 and RP2040
// clk_peri 252 MHz / 16 / 7.875, versus about -0.8% error on the STM32 at
// 1843200. Stock Flipper firmware still works at this rate (the HAL accepts
// 9600-4000000) but keeps the per-byte RX path, so pair this build with the
// FlipDev firmware. Set back to 921600 if errors return.
#define UART_BAUD_RATE (2000000UL)

#define EXPANSION_MODULE_TIMEOUT_MS (EXPANSION_PROTOCOL_TIMEOUT_MS - 50UL)
#define EXPANSION_MODULE_STARTUP_DELAY_MS (250UL)
// Room for several frames: with full-duplex operation (see "Link layer"
// below) a Status, a Data frame and a Heartbeat can all be in flight towards
// us while this task is busy writing its own frame. A full stream buffer
// silently drops bytes in the RX ISR, which desyncs the link.
#define EXPANSION_MODULE_BUFFER_SIZE (1024)
// How often a quiet link checks whether we have a request to send.
#define EXPANSION_MODULE_RX_SLICE_MS (5UL)
#define EXPANSION_MODULE_TX_QUEUE_LEN (16)

static StreamBufferHandle_t stream;
static PB_Main rpc_message;
static PB_Main tx_message;
static QueueHandle_t link_tx_queue;
static volatile bool link_up;
static LinkListener link_listener;
static uint32_t link_list_command_id; // outstanding storage list, 0 if none

static bool link_has_outbound(void);

// Set around a frame decode in expansion_pump(): when the link is quiet at a
// message boundary, return early (without consuming any byte) as soon as a
// request is queued, instead of sitting out the whole idle timeout.
static bool rx_at_frame_start;
static bool rx_interruptible;
static bool rx_interrupted;

// RX interrupt handler
static void uart_on_rx() {
    BaseType_t higher_priority_task_woken = pdFALSE;

    while(uart_is_readable(UART_ID)) {
        const uint8_t ch = uart_getc(UART_ID);
        xStreamBufferSendFromISR(stream, &ch, sizeof(ch), &higher_priority_task_woken);
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

// Receive frames
static size_t expansion_receive_callback(uint8_t* data, size_t data_size, void* context) {
    (void)context;
    size_t received_size = 0;

    if(rx_at_frame_start && rx_interruptible) {
        const TickType_t start = xTaskGetTickCount();
        while(true) {
            received_size = xStreamBufferReceive(
                stream, data, data_size, pdMS_TO_TICKS(EXPANSION_MODULE_RX_SLICE_MS));
            if(received_size) break;
            if(link_has_outbound()) {
                rx_interrupted = true;
                return 0;
            }
            if(xTaskGetTickCount() - start >= pdMS_TO_TICKS(EXPANSION_MODULE_TIMEOUT_MS)) {
                return 0;
            }
        }
    }

    while(received_size < data_size) {
        const size_t received_size_cur = xStreamBufferReceive(
            stream,
            data + received_size,
            data_size - received_size,
            pdMS_TO_TICKS(EXPANSION_MODULE_TIMEOUT_MS));
        if(received_size_cur == 0) break;
        received_size += received_size_cur;
    }

    if(received_size) rx_at_frame_start = false;
    return received_size;
}

static inline bool expansion_receive_frame(ExpansionFrame* frame) {
    return expansion_protocol_decode(frame, expansion_receive_callback, NULL) ==
           ExpansionProtocolStatusOk;
}

static inline bool expansion_is_heartbeat_frame(const ExpansionFrame* frame) {
    return frame->header.type == ExpansionFrameTypeHeartbeat;
}

static inline bool expansion_is_data_frame(const ExpansionFrame* frame) {
    return frame->header.type == ExpansionFrameTypeData;
}

static inline bool expansion_is_success_frame(const ExpansionFrame* frame) {
    return frame->header.type == ExpansionFrameTypeStatus &&
           frame->content.status.error == ExpansionFrameErrorNone;
}

// Send frames
static size_t expansion_send_callback(const uint8_t* data, size_t data_size, void* context) {
    (void)context;

    uart_write_blocking(UART_ID, data, data_size);
    return data_size;
}

static inline bool expansion_send_frame(const ExpansionFrame* frame) {
    return expansion_protocol_encode(frame, expansion_send_callback, NULL) ==
           ExpansionProtocolStatusOk;
}

static inline bool expansion_send_heartbeat() {
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeHeartbeat,
        .content.heartbeat = {},
    };

    return expansion_send_frame(&frame);
}

static inline bool expansion_send_status(ExpansionFrameError error) {
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeStatus,
        .content.status.error = error,
    };

    return expansion_send_frame(&frame);
}

static bool expansion_send_baud_rate_request(uint32_t baud_rate) {
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeBaudRate,
        .content.baud_rate.baud = baud_rate,
    };

    return expansion_send_frame(&frame);
}

static bool expansion_send_control_request(ExpansionFrameControlCommand command) {
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeControl,
        .content.control.command = command,
    };

    return expansion_send_frame(&frame);
}

static bool expansion_send_data_request(const uint8_t* data, size_t data_size) {
    ExpansionFrame frame = {
        .header.type = ExpansionFrameTypeData,
        .content.data.size = data_size,
    };

    memcpy(frame.content.data.bytes, data, data_size);
    return expansion_send_frame(&frame);
}

// Coprocessor channel (compute offload) - sibling to the RPC channel below,
// interleaved on the same link. See ExpansionFrameTypeCoproc in
// expansion_protocol.h. Keep expansion_handle_coproc_frame() in sync with
// expansion_worker_handle_coproc_frame() on the Flipper side.

static bool expansion_wait_status(void);

typedef struct {
    uint16_t job_id;
    uint16_t chunk_next;
    size_t len;
} CoprocRxState;

static CoprocRxState coproc_rx_state;

// Reassembles inbound job-request chunks into coproc's RX buffer and hands
// complete messages to compute_task. Always ACKs the frame; out-of-sequence
// or oversized messages are dropped and NAK'd rather than desyncing the link.
static void expansion_handle_coproc_frame(const ExpansionFrame* frame) {
    const ExpansionFrameCoproc* coproc = &frame->content.coproc;
    ExpansionFrameError ack_error = ExpansionFrameErrorNone;

    do {
        if(!(coproc->flags & ExpansionFrameCoprocFlagRequest)) break; // we don't submit jobs

        if(coproc->flags & ExpansionFrameCoprocFlagFirst) {
            coproc_rx_state.job_id = coproc->job_id;
            coproc_rx_state.chunk_next = 0;
            coproc_rx_state.len = 0;
        }

        if(coproc->job_id != coproc_rx_state.job_id ||
           coproc->chunk_seq != coproc_rx_state.chunk_next ||
           coproc->payload_len > COPROC_MAX_MESSAGE_SIZE - coproc_rx_state.len) {
            coproc_rx_state.chunk_next = 0;
            coproc_rx_state.len = 0;
            ack_error = ExpansionFrameErrorUnknown;
            break;
        }

        memcpy(
            coproc_get_rx_buffer() + coproc_rx_state.len, coproc->payload, coproc->payload_len);
        coproc_rx_state.len += coproc->payload_len;
        coproc_rx_state.chunk_next++;

        if(coproc->flags & ExpansionFrameCoprocFlagLast) {
            coproc_submit_job(coproc_rx_state.job_id, coproc->task_type, coproc_rx_state.len);
            coproc_rx_state.chunk_next = 0;
            coproc_rx_state.len = 0;
        }
    } while(false);

    expansion_send_status(ack_error);
}

// Sends a finished job result as chunked Coproc frames, waiting for a Status
// ack per chunk - same flow-control discipline as expansion_send_data_request().
static bool expansion_send_coproc_result(
    uint16_t job_id,
    uint8_t task_type,
    bool is_error,
    const uint8_t* data,
    size_t data_size) {
    const uint16_t chunk_total = data_size == 0 ?
                                      1 :
                                      (data_size + EXPANSION_PROTOCOL_COPROC_MAX_PAYLOAD_SIZE - 1) /
                                          EXPANSION_PROTOCOL_COPROC_MAX_PAYLOAD_SIZE;

    for(uint16_t chunk_seq = 0; chunk_seq < chunk_total; chunk_seq++) {
        const size_t offset = (size_t)chunk_seq * EXPANSION_PROTOCOL_COPROC_MAX_PAYLOAD_SIZE;
        const size_t chunk_len =
            MIN(data_size - offset, (size_t)EXPANSION_PROTOCOL_COPROC_MAX_PAYLOAD_SIZE);

        uint8_t flags = 0; // Request flag clear: this is a result, not a job submission
        if(chunk_seq == 0) flags |= ExpansionFrameCoprocFlagFirst;
        if(chunk_seq == chunk_total - 1) flags |= ExpansionFrameCoprocFlagLast;
        if(is_error) flags |= ExpansionFrameCoprocFlagError;

        ExpansionFrame frame = {
            .header.type = ExpansionFrameTypeCoproc,
            .content.coproc =
                {
                    .job_id = job_id,
                    .task_type = task_type,
                    .flags = flags,
                    .chunk_seq = chunk_seq,
                    .chunk_total = chunk_total,
                    .payload_len = chunk_len,
                },
        };
        memcpy(frame.content.coproc.payload, data + offset, chunk_len);

        if(!expansion_send_frame(&frame)) return false;
        if(!expansion_wait_status()) return false;
    }

    return true;
}

// Checks for a completed compute job and sends its result, if any. Called
// at message boundaries (expansion_flush_outbound) and on an idle gap in
// the link (expansion_pump). Returns false only if sending failed.
static bool expansion_coproc_service_pending_result(void) {
    static bool busy; // sending a result pumps the link, which may idle again
    uint16_t job_id;
    uint8_t task_type;
    bool is_error;
    size_t data_size;

    if(busy) return true;
    bool ok = true;
    if(coproc_poll_result(&job_id, &task_type, &is_error, &data_size)) {
        busy = true;
        ok = expansion_send_coproc_result(
            job_id, task_type, is_error, coproc_get_tx_buffer(), data_size);
        busy = false;
    }
    return ok;
}

// Link layer: full duplex on top of the stop-and-wait expansion protocol.
//
// Each side keeps at most one Data frame in flight until the other side's
// Status ack. The Flipper's expansion worker acks our Data from its own
// thread, independently of the RPC traffic it is sending, so its frames and
// ours can cross on the wire: while we wait for the ack to a request of ours,
// the next chunk of a screen frame may arrive first. So every inbound Data
// frame is acked as soon as it arrives and queued, in order, for the protobuf
// decoder; waiting for our own ack pumps the same receive path.
//
// The queue holds RX_FIFO_ACKED_MAX acked frames plus one held back without
// an ack. The Flipper cannot send another Data frame until it gets that ack,
// which is only sent once the decoder takes the frame - so a burst we can't
// buffer is throttled rather than dropped.

#define RX_FIFO_ACKED_MAX (16)
#define RX_FIFO_SIZE (RX_FIFO_ACKED_MAX + 1)

typedef struct {
    ExpansionFrame frame;
    bool acked;
} RxFifoEntry;

static RxFifoEntry rx_fifo[RX_FIFO_SIZE];
static uint8_t rx_fifo_head;
static uint8_t rx_fifo_count;

static void rx_fifo_reset(void) {
    rx_fifo_head = 0;
    rx_fifo_count = 0;
}

static bool rx_fifo_push(const ExpansionFrame* frame) {
    if(rx_fifo_count >= RX_FIFO_SIZE) return false; // peer ignored flow control
    const bool ack = rx_fifo_count < RX_FIFO_ACKED_MAX;
    if(ack && !expansion_send_status(ExpansionFrameErrorNone)) return false;
    RxFifoEntry* entry = &rx_fifo[(rx_fifo_head + rx_fifo_count) % RX_FIFO_SIZE];
    memcpy(&entry->frame, frame, sizeof(ExpansionFrame));
    entry->acked = ack;
    rx_fifo_count++;
    return true;
}

static bool rx_fifo_pop(ExpansionFrame* frame) {
    if(rx_fifo_count == 0) return false;
    RxFifoEntry* entry = &rx_fifo[rx_fifo_head];
    memcpy(frame, &entry->frame, sizeof(ExpansionFrame));
    const bool acked = entry->acked;
    rx_fifo_head = (rx_fifo_head + 1) % RX_FIFO_SIZE;
    rx_fifo_count--;
    return acked || expansion_send_status(ExpansionFrameErrorNone);
}

typedef enum {
    PumpData, // a Data frame was queued
    PumpStatusOk,
    PumpStatusError,
    PumpInterrupted, // quiet link and a request is waiting to be sent
    PumpError,
} PumpResult;

// Receives frames until one is meaningful to the caller. Heartbeats and
// Coproc job frames are handled here. On an idle gap (no byte for
// EXPANSION_MODULE_TIMEOUT_MS) it services a finished coproc job and sends a
// heartbeat; a second gap without the heartbeat answered means the link is
// gone.
//
// The coproc result is not checked on every frame: an earlier per-frame
// check slowed this hot path enough to provoke receive overruns on the
// Flipper side (before its RX moved to DMA). A ready result counts as
// outbound work instead (link_has_outbound), so it goes out at the next
// message boundary like a request; the idle-gap check here is a backstop.
//
// want_data: the caller is the protobuf decoder. Sending a coproc result
// pumps the link itself and may queue Data frames; hand those back instead
// of waiting on the UART for more, or the decoder would never see them (and
// a held, unacked frame would time the Flipper out). wait_status passes
// false: returning early there would reset heartbeat_pending on every call
// and defeat link-loss detection.
static PumpResult expansion_pump(bool interruptible, bool want_data) {
    bool heartbeat_pending = false;
    ExpansionFrame frame;

    while(true) {
        rx_at_frame_start = true;
        rx_interruptible = interruptible;
        rx_interrupted = false;
        const ExpansionProtocolStatus status =
            expansion_protocol_decode(&frame, expansion_receive_callback, NULL);
        rx_interruptible = false;

        if(status == ExpansionProtocolStatusErrorCommunication) {
            if(rx_interrupted) return PumpInterrupted;
            if(!rx_at_frame_start) return PumpError; // timed out mid-frame: bytes were lost
            if(!expansion_coproc_service_pending_result()) return PumpError;
            if(want_data && rx_fifo_count) return PumpData;
            if(heartbeat_pending || !expansion_send_heartbeat()) return PumpError;
            heartbeat_pending = true;
            continue;
        } else if(status != ExpansionProtocolStatusOk) {
            return PumpError;
        }

        switch(frame.header.type) {
        case ExpansionFrameTypeData:
            return rx_fifo_push(&frame) ? PumpData : PumpError;
        case ExpansionFrameTypeStatus:
            return frame.content.status.error == ExpansionFrameErrorNone ? PumpStatusOk :
                                                                            PumpStatusError;
        case ExpansionFrameTypeHeartbeat:
            heartbeat_pending = false;
            break;
        case ExpansionFrameTypeCoproc:
            expansion_handle_coproc_frame(&frame);
            break;
        default:
            return PumpError;
        }
    }
}

// Waits for the Status ack to the frame we just sent.
static bool expansion_wait_status(void) {
    while(true) {
        const PumpResult result = expansion_pump(false, false);
        if(result == PumpStatusOk) return true;
        if(result != PumpData) return false;
    }
}

// Rpc functions

typedef struct {
    ExpansionFrame frame;
    size_t read_size; // Number of bytes already read from the data frame
    bool interruptible; // may give up before the first byte of the message
    bool started; // at least one frame of the message was taken
    bool interrupted;
} ExpansionRpcContext;

static bool
    expansion_rpc_decode_callback(pb_istream_t* stream, pb_byte_t* data, size_t data_size) {
    ExpansionRpcContext* ctx = stream->state;
    size_t received_size = 0;

    while(received_size != data_size) {
        if(ctx->read_size == ctx->frame.content.data.size) {
            // Need the next Data frame of this message
            while(rx_fifo_count == 0) {
                const PumpResult result =
                    expansion_pump(ctx->interruptible && !ctx->started, true);
                if(result == PumpData) break;
                if(result == PumpInterrupted) ctx->interrupted = true;
                return false;
            }
            if(!rx_fifo_pop(&ctx->frame)) return false;
            ctx->read_size = 0;
            ctx->started = true;
            continue;
        }

        const size_t current_size =
            MIN(data_size - received_size, ctx->frame.content.data.size - ctx->read_size);
        memcpy(data + received_size, ctx->frame.content.data.bytes + ctx->read_size, current_size);

        ctx->read_size += current_size;
        received_size += current_size;
    }

    return true;
}

typedef enum {
    RpcRxOk,
    RpcRxInterrupted,
    RpcRxError,
} RpcRxResult;

static RpcRxResult expansion_receive_rpc_message_ex(PB_Main* message, bool interruptible) {
    ExpansionRpcContext ctx = {.interruptible = interruptible};

    pb_istream_t is = {
        .callback = expansion_rpc_decode_callback,
        .state = &ctx,
        .bytes_left = SIZE_MAX,
        .errmsg = NULL,
    };

    if(pb_decode_ex(&is, &PB_Main_msg, message, PB_DECODE_DELIMITED)) return RpcRxOk;
    return ctx.interrupted && !ctx.started ? RpcRxInterrupted : RpcRxError;
}

static bool expansion_receive_rpc_message(PB_Main* message) {
    return expansion_receive_rpc_message_ex(message, false) == RpcRxOk;
}

static bool
    expansion_rpc_encode_callback(pb_ostream_t* stream, const pb_byte_t* data, size_t data_size) {
    size_t sent_size = 0;

    while(sent_size != data_size) {
        const size_t current_size = MIN(data_size - sent_size, EXPANSION_PROTOCOL_MAX_DATA_SIZE);
        if(!expansion_send_data_request(data + sent_size, current_size)) break;
        // Data frames the Flipper sends meanwhile are acked and queued.
        if(!expansion_wait_status()) break;
        sent_size += current_size;
    }

    return (sent_size == data_size);
}

static bool expansion_send_rpc_message(PB_Main* message) {
    pb_ostream_t os = {
        .callback = expansion_rpc_encode_callback,
        .state = NULL,
        .max_size = SIZE_MAX,
        .bytes_written = 0,
        .errmsg = NULL,
    };

    const bool success = pb_encode_ex(&os, &PB_Main_msg, message, PB_ENCODE_DELIMITED);
    pb_release(&PB_Main_msg, message);
    return success;
}

static inline bool expansion_is_success_rpc_response(const PB_Main* message) {
    return message->command_status == PB_CommandStatus_OK &&
           message->which_content == PB_Main_empty_tag;
}

static inline bool expansion_is_input_rpc_response(const PB_Main* message) {
    return message->command_id == 0 && message->command_status == PB_CommandStatus_OK &&
           message->which_content == PB_Main_gui_send_input_event_request_tag;
}

static inline bool expansion_is_screen_frame_rpc_response(const PB_Main* message) {
    return message->command_id == 0 && message->command_status == PB_CommandStatus_OK &&
           message->which_content == PB_Main_gui_screen_frame_tag;
}

// Main states

static bool expansion_wait_ready() {
    bool success = false;

    do {
        ExpansionFrame frame;
        if(!expansion_receive_frame(&frame)) break;
        if(!expansion_is_heartbeat_frame(&frame)) break;
        success = true;
    } while(false);

    return success;
}

static bool expansion_handshake() {
    bool success = false;

    do {
        if(!expansion_send_baud_rate_request(UART_BAUD_RATE)) break;
        ExpansionFrame frame;
        if(!expansion_receive_frame(&frame)) break;
        if(!expansion_is_success_frame(&frame)) break;
        uart_set_baudrate(UART_ID, UART_BAUD_RATE);
        vTaskDelay(pdMS_TO_TICKS(EXPANSION_PROTOCOL_BAUD_CHANGE_DT_MS));
        success = true;
    } while(false);

    return success;
}

static bool expansion_start_rpc() {
    bool success = false;

    do {
        if(!expansion_send_control_request(ExpansionFrameControlCommandStartRpc)) break;
        ExpansionFrame frame;
        if(!expansion_receive_frame(&frame)) break;
        if(!expansion_is_success_frame(&frame)) break;
        success = true;
    } while(false);

    return success;
}

static uint32_t expansion_get_next_command_id() {
    static uint32_t command_id;
    while(++command_id == 0)
        ;
    return command_id;
}

static bool expansion_start_virtual_display() {
    bool success = false;

    rpc_message.command_id = expansion_get_next_command_id();
    rpc_message.command_status = PB_CommandStatus_OK;
    rpc_message.which_content = PB_Main_gui_start_virtual_display_request_tag;
    rpc_message.has_next = false;

    PB_Gui_StartVirtualDisplayRequest* request =
        &rpc_message.content.gui_start_virtual_display_request;

    request->send_input = true;
    request->has_first_frame = true;
    request->first_frame.orientation = PB_Gui_ScreenOrientation_HORIZONTAL;
    request->first_frame.data = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(FLIPPER_BITMAP_SIZE));
    // Was unchecked (stock code) - a NULL here under heap pressure would
    // otherwise be an immediate crash on the very next line, at exactly the
    // point where this firmware's first real allocation-heavy step runs.
    if(!request->first_frame.data) return false;
    request->first_frame.data->size = FLIPPER_BITMAP_SIZE;

    memcpy(request->first_frame.data->bytes, bitmap_splash_screen, FLIPPER_BITMAP_SIZE);

    do {
        if(!expansion_send_rpc_message(&rpc_message)) break;
        if(!expansion_receive_rpc_message(&rpc_message)) break;
        if(!expansion_is_success_rpc_response(&rpc_message)) break;

        // Show the same picture on display
        uint8_t* frame_buffer = calloc(FLIPPER_BITMAP_SIZE, sizeof(uint8_t));

        bitmap_xbm_to_screen_frame(
            frame_buffer, bitmap_splash_screen, FLIPPER_SCREEN_WIDTH, FLIPPER_SCREEN_HEIGHT);
        frame_parse_data(
            OrientationHorizontal,
            (const frame_t*)frame_buffer,
            pdMS_TO_TICKS(EXPANSION_MODULE_TIMEOUT_MS));

        free(frame_buffer);
        success = true;
    } while(false);

    pb_release(&PB_Main_msg, &rpc_message);
    return success;
}

static bool expansion_wait_input() {
    bool success = false;

    while(!success) {
        if(!expansion_receive_rpc_message(&rpc_message)) break;
        if(!expansion_is_input_rpc_response(&rpc_message)) break;

        const PB_Gui_SendInputEventRequest* request =
            &rpc_message.content.gui_send_input_event_request;
        success = (request->type == PB_Gui_InputType_RELEASE);
        pb_release(&PB_Main_msg, &rpc_message);
    };

    pb_release(&PB_Main_msg, &rpc_message);
    return success;
}

static bool expansion_stop_virtual_display() {
    bool success = false;

    rpc_message.command_id = expansion_get_next_command_id();
    rpc_message.command_status = PB_CommandStatus_OK;
    rpc_message.which_content = PB_Main_gui_stop_virtual_display_request_tag;
    rpc_message.has_next = false;

    if(expansion_send_rpc_message(&rpc_message)) {
        while(true) {
            if(!expansion_receive_rpc_message(&rpc_message)) break;
            if(!expansion_is_input_rpc_response(&rpc_message)) {
                success = expansion_is_success_rpc_response(&rpc_message);
                break;
            }

            pb_release(&PB_Main_msg, &rpc_message);
        }
    }

    pb_release(&PB_Main_msg, &rpc_message);
    return success;
}

static bool expansion_start_screen_streaming() {
    bool success = false;

    rpc_message.command_id = expansion_get_next_command_id();
    rpc_message.command_status = PB_CommandStatus_OK;
    rpc_message.which_content = PB_Main_gui_start_screen_stream_request_tag;
    rpc_message.has_next = false;

    do {
        if(!expansion_send_rpc_message(&rpc_message)) break;
        if(!expansion_receive_rpc_message(&rpc_message)) break;
        if(!expansion_is_success_rpc_response(&rpc_message)) break;
        success = true;
    } while(false);

    pb_release(&PB_Main_msg, &rpc_message);
    return success;
}

// Outbound requests (keyboard input, pings) ------------------------------

#ifdef VGM_LINK_PING_TEST
// Link soak test: a system_ping_request every VGM_LINK_PING_TEST ms while the
// screen streams, so our requests constantly cross the Flipper's frames. The
// Flipper logs "Ping" at debug level for each one it handles.
static TickType_t ping_last;
static uint32_t ping_sent, ping_ok, ping_bad;

static bool ping_due(void) {
    return xTaskGetTickCount() - ping_last >= pdMS_TO_TICKS(VGM_LINK_PING_TEST);
}
#endif

static bool link_has_outbound(void) {
#ifdef VGM_LINK_PING_TEST
    if(ping_due()) return true;
#endif
    if(coproc_result_ready()) return true;
    return link_tx_queue && uxQueueMessagesWaiting(link_tx_queue) > 0;
}

bool uart_link_submit(const LinkRequest* request) {
    if(!link_tx_queue || !link_up) return false;
    return xQueueSend(link_tx_queue, request, 0) == pdTRUE;
}

bool uart_link_is_up(void) {
    return link_up;
}

void uart_link_set_listener(LinkListener listener) {
    link_listener = listener;
}

static void link_emit(const LinkEvent* event) {
    if(link_listener) link_listener(event);
}

static void link_set_up(bool up) {
    if(link_up == up) return;
    link_up = up;
    const LinkEvent event = {.type = up ? LinkEventUp : LinkEventDown};
    link_emit(&event);
}

__attribute__((weak)) void uart_link_on_screen_frame(uint8_t orientation, const uint8_t* data) {
    frame_parse_data(
        orientation, (const frame_t*)data, pdMS_TO_TICKS(EXPANSION_MODULE_TIMEOUT_MS));
}

static bool expansion_send_request(const LinkRequest* request) {
    memset(&tx_message, 0, sizeof(tx_message));
    tx_message.command_id = expansion_get_next_command_id();
    tx_message.command_status = PB_CommandStatus_OK;
    tx_message.has_next = false;

    switch(request->type) {
    case LinkRequestPing:
        tx_message.which_content = PB_Main_system_ping_request_tag;
        break;
    case LinkRequestInput:
        tx_message.which_content = PB_Main_gui_send_input_event_request_tag;
        tx_message.content.gui_send_input_event_request.key = request->input.key;
        tx_message.content.gui_send_input_event_request.type = request->input.type;
        break;
    case LinkRequestDateTime:
        tx_message.which_content = PB_Main_system_get_datetime_request_tag;
        break;
    case LinkRequestPowerInfo:
        tx_message.which_content = PB_Main_system_power_info_request_tag;
        break;
    case LinkRequestStorageList:
        tx_message.which_content = PB_Main_storage_list_request_tag;
        // pb_release() in expansion_send_rpc_message() frees pointer fields
        tx_message.content.storage_list_request.path = strdup(request->path);
        if(!tx_message.content.storage_list_request.path) return true; // drop
        link_list_command_id = tx_message.command_id;
        break;
    case LinkRequestAppStart:
        tx_message.which_content = PB_Main_app_start_request_tag;
        tx_message.content.app_start_request.name = strdup(request->app.name);
        tx_message.content.app_start_request.args = strdup(request->app.args);
        if(!tx_message.content.app_start_request.name ||
           !tx_message.content.app_start_request.args) {
            free(tx_message.content.app_start_request.name);
            free(tx_message.content.app_start_request.args);
            return true; // drop
        }
        break;
    default:
        return true; // unknown: drop
    }

    return expansion_send_rpc_message(&tx_message);
}

static bool expansion_flush_outbound(void) {
    if(!expansion_coproc_service_pending_result()) return false;
#ifdef VGM_LINK_PING_TEST
    if(ping_due()) {
        ping_last = xTaskGetTickCount();
        const LinkRequest ping = {.type = LinkRequestPing};
        if(!expansion_send_request(&ping)) return false;
        if(++ping_sent % 100 == 0) {
            printf("link ping: sent %lu ok %lu bad %lu\n", ping_sent, ping_ok, ping_bad);
        }
    }
#endif
    LinkRequest request;
    while(xQueueReceive(link_tx_queue, &request, 0) == pdTRUE) {
        if(!expansion_send_request(&request)) return false;
    }
    return true;
}

// Answers to our requests arrive interleaved with screen frames.
static void expansion_handle_command_response(const PB_Main* message) {
#ifdef VGM_LINK_PING_TEST
    if(message->which_content == PB_Main_system_ping_response_tag &&
       message->command_status == PB_CommandStatus_OK) {
        ping_ok++;
        return;
    } else if(message->which_content == PB_Main_system_ping_response_tag) {
        ping_bad++;
        return;
    }
#endif
    LinkEvent event;
    if(message->command_id == link_list_command_id) {
        if(message->command_status != PB_CommandStatus_OK) {
            event.type = LinkEventListError;
            link_emit(&event);
            link_list_command_id = 0;
            return;
        }
        if(message->which_content == PB_Main_storage_list_response_tag) {
            const PB_Storage_ListResponse* list = &message->content.storage_list_response;
            for(pb_size_t i = 0; i < list->file_count; i++) {
                event.type = LinkEventListEntry;
                event.entry.name = list->file[i].name ? list->file[i].name : "";
                event.entry.is_dir = list->file[i].type == PB_Storage_File_FileType_DIR;
                event.entry.size = list->file[i].size;
                link_emit(&event);
            }
        }
        if(!message->has_next) {
            event.type = LinkEventListDone;
            link_emit(&event);
            link_list_command_id = 0;
        }
        return;
    }

    if(message->command_status != PB_CommandStatus_OK) {
        printf("rpc: command %lu failed: %d\n", message->command_id, message->command_status);
        return;
    }

    if(message->which_content == PB_Main_system_get_datetime_response_tag &&
       message->content.system_get_datetime_response.has_datetime) {
        event.type = LinkEventDateTime;
        event.datetime.hour = message->content.system_get_datetime_response.datetime.hour;
        event.datetime.minute = message->content.system_get_datetime_response.datetime.minute;
        link_emit(&event);
    } else if(
        message->which_content == PB_Main_system_power_info_response_tag &&
        message->content.system_power_info_response.key) {
        event.type = LinkEventPower;
        event.power.key = message->content.system_power_info_response.key;
        event.power.value = message->content.system_power_info_response.value ?
                                message->content.system_power_info_response.value :
                                "";
        link_emit(&event);
    }
}

static void expansion_process_screen_streaming() {
    while(true) {
        if(!expansion_flush_outbound()) break;

        const RpcRxResult result = expansion_receive_rpc_message_ex(&rpc_message, true);
        if(result == RpcRxInterrupted) continue;
        if(result != RpcRxOk) break;

        if(expansion_is_screen_frame_rpc_response(&rpc_message)) {
            // Display frame
            const PB_Gui_ScreenOrientation orientation =
                rpc_message.content.gui_screen_frame.orientation;
            const pb_byte_t* data = rpc_message.content.gui_screen_frame.data->bytes;

            uart_link_on_screen_frame(orientation, data);
        } else if(rpc_message.command_id != 0) {
            expansion_handle_command_response(&rpc_message);
        }
        // Anything else unsolicited is ignored rather than dropping the link.

        pb_release(&PB_Main_msg, &rpc_message);
    }

    pb_release(&PB_Main_msg, &rpc_message);
}

static void uart_task(void* unused_arg) {
    // startup delay (skip potential module insertion interference)
    vTaskDelay(pdMS_TO_TICKS(EXPANSION_MODULE_STARTUP_DELAY_MS));
    // init stream buffer
    stream = xStreamBufferCreate(EXPANSION_MODULE_BUFFER_SIZE, 1);
    assert(stream != NULL);
    link_tx_queue = xQueueCreate(EXPANSION_MODULE_TX_QUEUE_LEN, sizeof(LinkRequest));
    assert(link_tx_queue != NULL);

    // init uart 0
    uart_init(uart0, UART_INIT_BAUD_RATE);

    // init uart gpio
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    gpio_set_pulls(UART_RX_PIN, true, false);
    gpio_set_pulls(UART_TX_PIN, true, false);

    // enable uart fifo
    uart_set_fifo_enabled(UART_ID, true);

    // config uart for 8N1 transmission
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);

    // disable hardware flow control
    uart_set_hw_flow(UART_ID, false, false);

    // add uart irqs
    irq_set_exclusive_handler(UART_IRQ, uart_on_rx);
    irq_set_enabled(UART_IRQ, true);

    // enable rx irq
    uart_set_irq_enables(UART_ID, true, false);

    // show splash screen only once per power-up
    bool splash_screen_shown = false;

    while(true) {
        link_set_up(false);
        link_list_command_id = 0;
        xQueueReset(link_tx_queue);
        rx_fifo_reset();

        // leds: activate waiting state
        led_state_wait();

        // reset baud rate to initial value
        uart_set_baudrate(UART_ID, UART_INIT_BAUD_RATE);
        // announce presence (one pulse high -> low)
        uart_putc_raw(UART_ID, 0xF0);

        // wait for host response
        if(!expansion_wait_ready()) continue;
        // negotiate baud rate
        if(!expansion_handshake()) continue;
        // start rpc
        if(!expansion_start_rpc()) continue;

        // leds: activate active state
        led_state_active();

        if(!splash_screen_shown) {
            // start virtual display
            if(!expansion_start_virtual_display()) continue;
            // wait for button press
            if(!expansion_wait_input()) continue;
            // stop virtual display
            if(!expansion_stop_virtual_display()) continue;

            splash_screen_shown = true;
        }
        // start screen streaming
        if(!expansion_start_screen_streaming()) continue;
        link_set_up(true);
        // process screen frame messages - returns only on error
        expansion_process_screen_streaming();
    }
}

void uart_protocol_init(void) {
    TaskHandle_t uart_task_handle = NULL;
    BaseType_t status = xTaskCreate(uart_task, "uart_task", 4 * 1024, NULL, 1, &uart_task_handle);
    assert(status == pdPASS);
    (void)status;
}

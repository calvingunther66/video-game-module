#pragma once
/*
 * Remote GPIO expander for the COPROC_TASK_GPIO task: lets the Flipper drive
 * the VGM's breakout GPIO over the coproc link.
 *
 * Job payload: N records of 3 bytes, [u8 op][u8 pin][u8 arg].
 * Result: N records of 3 bytes, [u8 status][u16 value LE], in the same order.
 *
 * Must stay in sync with CoprocGpioOp / CoprocGpioMode / CoprocGpioStatus in
 * Momentum's applications/services/coproc/coproc.h.
 *
 * Only the breakout pins are reachable: GPIO 16, 17 and 21-29 (ADC on
 * 26-29). Anything else, including the expansion UART (0/1) and the PicoDVI
 * pins (8-15), is refused per record with COPROC_GPIO_STATUS_BAD_PIN.
 * GPIO 21, 24 and 25 double as the RGB LED, so the LED status code may
 * overwrite what the Flipper writes there.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COPROC_GPIO_RECORD_SIZE 3

// Ops
#define COPROC_GPIO_OP_MODE  0 // arg: COPROC_GPIO_MODE_*
#define COPROC_GPIO_OP_WRITE 1 // arg: 0 = low, else high. Pin must be in output mode.
#define COPROC_GPIO_OP_READ  2 // arg ignored; value = 0/1
#define COPROC_GPIO_OP_ADC   3 // arg ignored; value = raw 12-bit sample (3.3 V full scale)

// Modes for COPROC_GPIO_OP_MODE
#define COPROC_GPIO_MODE_INPUT     0
#define COPROC_GPIO_MODE_OUTPUT    1
#define COPROC_GPIO_MODE_PULL_UP   2
#define COPROC_GPIO_MODE_PULL_DOWN 3

// Per-record status
#define COPROC_GPIO_STATUS_OK        0
#define COPROC_GPIO_STATUS_BAD_PIN   1 // pin not on the breakout whitelist
#define COPROC_GPIO_STATUS_BAD_OP    2 // unknown op
#define COPROC_GPIO_STATUS_BAD_ARG   3 // unknown mode
#define COPROC_GPIO_STATUS_NO_ADC    4 // ADC op on a pin without an ADC channel
#define COPROC_GPIO_STATUS_NOT_OUTPUT 5 // write to a pin that isn't in output mode

// Runs every record in `in`. Returns false only for a malformed job (empty,
// not a whole number of records, or a result that wouldn't fit out_cap);
// per-record refusals are reported in the status bytes instead.
bool coproc_gpio_run(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size);

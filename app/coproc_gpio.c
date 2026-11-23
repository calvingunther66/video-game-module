#include "coproc_gpio.h"

#include <hardware/gpio.h>
#include <hardware/adc.h>

#define COPROC_GPIO_ADC_FIRST_PIN 26
#define COPROC_GPIO_ADC_LAST_PIN  29

// Breakout pins only (X7: GPIO 16, 17, 21-29). This is the one place that
// decides what the Flipper may touch, so the link UART (0/1) and the PicoDVI
// pins (8-15) can never be reconfigured from the other end.
static bool coproc_gpio_pin_allowed(uint8_t pin) {
    return pin == 16 || pin == 17 || (pin >= 21 && pin <= 29);
}

static bool coproc_gpio_pin_has_adc(uint8_t pin) {
    return pin >= COPROC_GPIO_ADC_FIRST_PIN && pin <= COPROC_GPIO_ADC_LAST_PIN;
}

static uint8_t coproc_gpio_set_mode(uint8_t pin, uint8_t mode) {
    if(mode > COPROC_GPIO_MODE_PULL_DOWN) return COPROC_GPIO_STATUS_BAD_ARG;

    // gpio_init() hands the pin to SIO as an input driving low, which also
    // undoes a previous adc_gpio_init() (that one turns the digital input off).
    gpio_init(pin);
    gpio_set_input_enabled(pin, true);
    switch(mode) {
    case COPROC_GPIO_MODE_OUTPUT:
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_OUT);
        break;
    case COPROC_GPIO_MODE_PULL_UP:
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
        break;
    case COPROC_GPIO_MODE_PULL_DOWN:
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
        break;
    default: // COPROC_GPIO_MODE_INPUT
        gpio_set_dir(pin, GPIO_IN);
        gpio_disable_pulls(pin);
        break;
    }
    return COPROC_GPIO_STATUS_OK;
}

static uint8_t coproc_gpio_adc_read(uint8_t pin, uint16_t* value) {
    static bool adc_ready = false;
    if(!coproc_gpio_pin_has_adc(pin)) return COPROC_GPIO_STATUS_NO_ADC;

    if(!adc_ready) {
        adc_init();
        adc_ready = true;
    }
    // Analog mode: digital input and pulls off. A later MODE op turns the
    // pin back into a digital one.
    adc_gpio_init(pin);
    adc_select_input(pin - COPROC_GPIO_ADC_FIRST_PIN);
    *value = adc_read();
    return COPROC_GPIO_STATUS_OK;
}

static uint8_t coproc_gpio_run_record(uint8_t op, uint8_t pin, uint8_t arg, uint16_t* value) {
    *value = 0;
    if(!coproc_gpio_pin_allowed(pin)) return COPROC_GPIO_STATUS_BAD_PIN;

    switch(op) {
    case COPROC_GPIO_OP_MODE:
        return coproc_gpio_set_mode(pin, arg);
    case COPROC_GPIO_OP_WRITE:
        if(gpio_get_function(pin) != GPIO_FUNC_SIO || !gpio_is_dir_out(pin))
            return COPROC_GPIO_STATUS_NOT_OUTPUT;
        gpio_put(pin, arg != 0);
        return COPROC_GPIO_STATUS_OK;
    case COPROC_GPIO_OP_READ:
        *value = gpio_get(pin) ? 1 : 0;
        return COPROC_GPIO_STATUS_OK;
    case COPROC_GPIO_OP_ADC:
        return coproc_gpio_adc_read(pin, value);
    default:
        return COPROC_GPIO_STATUS_BAD_OP;
    }
}

bool coproc_gpio_run(
    const uint8_t* in,
    size_t in_size,
    uint8_t* out,
    size_t out_cap,
    size_t* out_size) {
    if(in_size == 0 || in_size % COPROC_GPIO_RECORD_SIZE != 0) return false;
    if(in_size > out_cap) return false; // one 3-byte result per 3-byte record

    for(size_t i = 0; i < in_size; i += COPROC_GPIO_RECORD_SIZE) {
        uint16_t value = 0;
        out[i] = coproc_gpio_run_record(in[i], in[i + 1], in[i + 2], &value);
        out[i + 1] = (uint8_t)(value & 0xFF);
        out[i + 2] = (uint8_t)(value >> 8);
    }
    *out_size = in_size;
    return true;
}

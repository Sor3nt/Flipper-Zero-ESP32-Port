/**
 * @file furi_hal_gpio.c
 * GPIO HAL for ESP32
 */

#include "furi_hal_gpio.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>

static const char* TAG = "FuriHalGpio";

static GpioInterrupt furi_hal_gpio_interrupts[GPIO_NUM_MAX] = {0};
static bool furi_hal_gpio_isr_service_ready = false;
static const char* volatile furi_hal_gpio_reservations[GPIO_NUM_MAX] = {0};
static portMUX_TYPE furi_hal_gpio_reservation_lock = portMUX_INITIALIZER_UNLOCKED;

static bool furi_hal_gpio_is_valid(const GpioPin* gpio) {
    return gpio && gpio->pin != UINT16_MAX && gpio->pin < GPIO_NUM_MAX;
}

static void furi_hal_gpio_isr_handler(void* arg) {
    const uint32_t pin = (uint32_t)(uintptr_t)arg;
    if(pin >= GPIO_NUM_MAX) return;

    /* Registration and removal run on another core. add/remove write the callback
     * pair in a fixed order (see below), so reading the context first and the
     * callback second means a non-NULL callback always comes with the context it
     * was registered with, never with a stale or cleared one. */
    void* context = __atomic_load_n(&furi_hal_gpio_interrupts[pin].context, __ATOMIC_ACQUIRE);
    GpioExtiCallback callback =
        __atomic_load_n(&furi_hal_gpio_interrupts[pin].callback, __ATOMIC_ACQUIRE);
    if(callback) callback(context);
}

static void furi_hal_gpio_ensure_isr_service(void) {
    if(furi_hal_gpio_isr_service_ready) return;

    esp_err_t err = gpio_install_isr_service(0);
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        furi_hal_gpio_isr_service_ready = true;
    }
}

static gpio_mode_t furi_hal_gpio_convert_mode(GpioMode mode) {
    switch(mode) {
    case GpioModeInput:
    case GpioModeInterruptRise:
    case GpioModeInterruptFall:
    case GpioModeInterruptRiseFall:
    case GpioModeEventRise:
    case GpioModeEventFall:
    case GpioModeEventRiseFall:
        return GPIO_MODE_INPUT;
    case GpioModeOutputPushPull:
        return GPIO_MODE_OUTPUT;
    case GpioModeOutputOpenDrain:
        return GPIO_MODE_OUTPUT_OD;
    case GpioModeAltFunctionPushPull:
        return GPIO_MODE_INPUT_OUTPUT;
    case GpioModeAltFunctionOpenDrain:
        return GPIO_MODE_INPUT_OUTPUT_OD;
    case GpioModeAnalog:
    default:
        return GPIO_MODE_DISABLE;
    }
}

static gpio_int_type_t furi_hal_gpio_convert_interrupt(GpioMode mode) {
    switch(mode) {
    case GpioModeInterruptRise:
    case GpioModeEventRise:
        return GPIO_INTR_POSEDGE;
    case GpioModeInterruptFall:
    case GpioModeEventFall:
        return GPIO_INTR_NEGEDGE;
    case GpioModeInterruptRiseFall:
    case GpioModeEventRiseFall:
        return GPIO_INTR_ANYEDGE;
    default:
        return GPIO_INTR_DISABLE;
    }
}

void furi_hal_gpio_init_simple(const GpioPin* gpio, const GpioMode mode) {
    furi_hal_gpio_init(gpio, mode, GpioPullNo, GpioSpeedLow);
}

void furi_hal_gpio_init(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed) {
    furi_hal_gpio_init_ex(gpio, mode, pull, speed, GpioAltFnUnused);
}

static void furi_hal_gpio_init_for(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const char* caller) {
    if(!furi_hal_gpio_is_valid(gpio)) return;

    const char* owner = furi_hal_gpio_reservations[gpio->pin];
    if(owner && owner != caller) {
        ESP_LOGW(TAG, "GPIO%u is in use by %s; configuration ignored", gpio->pin, owner);
        return;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio->pin,
        .mode = furi_hal_gpio_convert_mode(mode),
        .pull_up_en = pull == GpioPullUp,
        .pull_down_en = pull == GpioPullDown,
        .intr_type = furi_hal_gpio_convert_interrupt(mode),
    };
    const esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%u: gpio_config failed: %s", gpio->pin, esp_err_to_name(err));
    }
}

void furi_hal_gpio_init_ex(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed,
    const GpioAltFn alt_fn) {
    (void)speed;
    (void)alt_fn;
    furi_hal_gpio_init_for(gpio, mode, pull, NULL);
}

void furi_hal_gpio_init_owned(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed,
    const char* owner) {
    (void)speed;
    furi_hal_gpio_init_for(gpio, mode, pull, owner);
}

void furi_hal_gpio_add_int_callback(const GpioPin* gpio, GpioExtiCallback cb, void* ctx) {
    if(!furi_hal_gpio_is_valid(gpio)) return;

    furi_hal_gpio_ensure_isr_service();
    if(!furi_hal_gpio_isr_service_ready) {
        ESP_LOGE(
            TAG, "GPIO%u: interrupt service unavailable, callback NOT registered", gpio->pin);
        return;
    }

    /* Detach the pin before the callback pair is rewritten, then publish the context
     * before the callback (the ISR reads them in the opposite order). */
    gpio_intr_disable((gpio_num_t)gpio->pin);
    gpio_isr_handler_remove((gpio_num_t)gpio->pin);
    __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].callback, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].context, ctx, __ATOMIC_RELEASE);
    __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].callback, cb, __ATOMIC_RELEASE);

    const esp_err_t err = gpio_isr_handler_add(
        (gpio_num_t)gpio->pin, furi_hal_gpio_isr_handler, (void*)(uintptr_t)gpio->pin);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%u: gpio_isr_handler_add failed: %s", gpio->pin, esp_err_to_name(err));
        __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].callback, NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].context, NULL, __ATOMIC_RELEASE);
        return;
    }
    gpio_intr_enable((gpio_num_t)gpio->pin);
}

void furi_hal_gpio_enable_int_callback(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return;
    gpio_intr_enable((gpio_num_t)gpio->pin);
}

void furi_hal_gpio_disable_int_callback(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return;
    gpio_intr_disable((gpio_num_t)gpio->pin);
}

void furi_hal_gpio_remove_int_callback(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return;

    gpio_intr_disable((gpio_num_t)gpio->pin);
    if(furi_hal_gpio_isr_service_ready) {
        gpio_isr_handler_remove((gpio_num_t)gpio->pin);
    }
    /* Callback first, context second: see furi_hal_gpio_isr_handler(). */
    __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].callback, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&furi_hal_gpio_interrupts[gpio->pin].context, NULL, __ATOMIC_RELEASE);
}

void furi_hal_gpio_write(const GpioPin* gpio, const bool state) {
    if(!furi_hal_gpio_is_valid(gpio)) return;
    gpio_set_level((gpio_num_t)gpio->pin, state ? 1 : 0);
}

bool furi_hal_gpio_read(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;
    return gpio_get_level((gpio_num_t)gpio->pin) != 0;
}

bool furi_hal_gpio_is_input(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;

    bool pull_up;
    bool pull_down;
    bool input_enabled;
    bool output_enabled;
    bool open_drain;
    bool sleep_select;
    uint32_t drive;
    uint32_t function;
    uint32_t output_signal;
    gpio_ll_get_io_config(
        &GPIO,
        gpio->pin,
        &pull_up,
        &pull_down,
        &input_enabled,
        &output_enabled,
        &open_drain,
        &drive,
        &function,
        &output_signal,
        &sleep_select);
    return input_enabled;
}

bool furi_hal_gpio_is_output(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;

    bool pull_up;
    bool pull_down;
    bool input_enabled;
    bool output_enabled;
    bool open_drain;
    bool sleep_select;
    uint32_t drive;
    uint32_t function;
    uint32_t output_signal;
    gpio_ll_get_io_config(
        &GPIO,
        gpio->pin,
        &pull_up,
        &pull_down,
        &input_enabled,
        &output_enabled,
        &open_drain,
        &drive,
        &function,
        &output_signal,
        &sleep_select);
    return output_enabled;
}

bool furi_hal_gpio_reserve(const GpioPin* gpio, const char* owner) {
    if(!furi_hal_gpio_is_valid(gpio) || !owner) return false;

    portENTER_CRITICAL(&furi_hal_gpio_reservation_lock);
    const char* current = furi_hal_gpio_reservations[gpio->pin];
    const bool reserved = current == NULL || current == owner;
    if(reserved) furi_hal_gpio_reservations[gpio->pin] = owner;
    portEXIT_CRITICAL(&furi_hal_gpio_reservation_lock);
    return reserved;
}

void furi_hal_gpio_unreserve(const GpioPin* gpio, const char* owner) {
    if(!furi_hal_gpio_is_valid(gpio)) return;

    portENTER_CRITICAL(&furi_hal_gpio_reservation_lock);
    if(furi_hal_gpio_reservations[gpio->pin] == owner) {
        furi_hal_gpio_reservations[gpio->pin] = NULL;
    }
    portEXIT_CRITICAL(&furi_hal_gpio_reservation_lock);
}

const char* furi_hal_gpio_get_reservation(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return NULL;
    return furi_hal_gpio_reservations[gpio->pin];
}

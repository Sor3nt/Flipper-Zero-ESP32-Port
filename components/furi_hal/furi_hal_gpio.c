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
#if !CONFIG_FREERTOS_UNICORE
#include <esp_ipc.h>
#endif

static const char* TAG = "FuriHalGpio";

static GpioInterrupt furi_hal_gpio_interrupts[GPIO_NUM_MAX] = {0};
static bool furi_hal_gpio_isr_service_ready = false;
static portMUX_TYPE furi_hal_gpio_isr_service_lock = portMUX_INITIALIZER_UNLOCKED;
#if !CONFIG_FREERTOS_UNICORE
/* Core the GPIO interrupt service (and so every GPIO ISR) is allocated on. */
static BaseType_t furi_hal_gpio_isr_core = 0;
#endif
static const char* volatile furi_hal_gpio_reservations[GPIO_NUM_MAX] = {0};
static portMUX_TYPE furi_hal_gpio_reservation_lock = portMUX_INITIALIZER_UNLOCKED;

static bool furi_hal_gpio_is_valid(const GpioPin* gpio) {
    return gpio && gpio->pin != UINT16_MAX && gpio->pin < GPIO_NUM_MAX;
}

static void furi_hal_gpio_isr_handler(void* arg) {
    const uint32_t pin = (uint32_t)(uintptr_t)arg;
    if(pin >= GPIO_NUM_MAX) return;

    /* add/remove may run on the other core. They write the callback pair in a
     * fixed order (see below), so reading the context first and the callback
     * second means a non-NULL callback always comes with its own context. */
    void* context = __atomic_load_n(&furi_hal_gpio_interrupts[pin].context, __ATOMIC_ACQUIRE);
    GpioExtiCallback callback =
        __atomic_load_n(&furi_hal_gpio_interrupts[pin].callback, __ATOMIC_ACQUIRE);
    if(callback) callback(context);
}

void furi_hal_gpio_init_isr_service(void) {
    portENTER_CRITICAL(&furi_hal_gpio_isr_service_lock);
    const bool ready = furi_hal_gpio_isr_service_ready;
    portEXIT_CRITICAL(&furi_hal_gpio_isr_service_lock);
    if(ready) return;

    esp_err_t err = gpio_install_isr_service(0);
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        portENTER_CRITICAL(&furi_hal_gpio_isr_service_lock);
#if !CONFIG_FREERTOS_UNICORE
        if(err == ESP_OK) furi_hal_gpio_isr_core = xPortGetCoreID();
#endif
        furi_hal_gpio_isr_service_ready = true;
        portEXIT_CRITICAL(&furi_hal_gpio_isr_service_lock);
    } else {
        ESP_LOGE(TAG, "GPIO interrupt service install failed: %s", esp_err_to_name(err));
    }
}

#if !CONFIG_FREERTOS_UNICORE
static void furi_hal_gpio_isr_sync_noop(void* arg) {
    (void)arg;
}
#endif

/* After a handler is detached, a GPIO ISR already running on the other core
 * may still be inside the old callback. An IPC call to that core only returns
 * once it has run a task again, i.e. after any in-flight ISR has returned, so
 * the caller can safely free the callback context afterwards. */
static void furi_hal_gpio_isr_sync(void) {
#if !CONFIG_FREERTOS_UNICORE
    const BaseType_t isr_core = furi_hal_gpio_isr_core;
    if(isr_core != xPortGetCoreID()) {
        esp_ipc_call_blocking(isr_core, furi_hal_gpio_isr_sync_noop, NULL);
    }
#endif
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
    gpio_config(&config);
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

    furi_hal_gpio_init_isr_service();
    if(!furi_hal_gpio_isr_service_ready) return;

    /* Detach the pin before the callback pair is rewritten, then publish the
     * context before the callback (the ISR reads them in the opposite order). */
    gpio_intr_disable((gpio_num_t)gpio->pin);
    gpio_isr_handler_remove((gpio_num_t)gpio->pin);
    furi_hal_gpio_isr_sync();
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
    /* Return only once no ISR can still be using the old context. */
    furi_hal_gpio_isr_sync();
}

void furi_hal_gpio_write(const GpioPin* gpio, const bool state) {
    if(!furi_hal_gpio_is_valid(gpio)) return;
    gpio_set_level((gpio_num_t)gpio->pin, state ? 1 : 0);
}

bool furi_hal_gpio_read(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;
    return gpio_get_level((gpio_num_t)gpio->pin) != 0;
}

static void furi_hal_gpio_get_io_config(
    const GpioPin* gpio,
    bool* input_enabled,
    bool* output_enabled) {
    bool pull_up;
    bool pull_down;
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
        input_enabled,
        output_enabled,
        &open_drain,
        &drive,
        &function,
        &output_signal,
        &sleep_select);
}

bool furi_hal_gpio_is_input(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;
    bool input_enabled = false;
    bool output_enabled = false;
    furi_hal_gpio_get_io_config(gpio, &input_enabled, &output_enabled);
    return input_enabled;
}

bool furi_hal_gpio_is_output(const GpioPin* gpio) {
    if(!furi_hal_gpio_is_valid(gpio)) return false;
    bool input_enabled = false;
    bool output_enabled = false;
    furi_hal_gpio_get_io_config(gpio, &input_enabled, &output_enabled);
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

/**
 * @file input_wake.c
 * Light-sleep GPIO wake for the board input drivers, see input_wake.h.
 */

#include "input_wake.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "InputWake"

static SemaphoreHandle_t input_wake_sem = NULL;
static StaticSemaphore_t input_wake_sem_buffer;
static gpio_num_t input_wake_pins[INPUT_WAKE_MAX_PINS];
static size_t input_wake_pin_count = 0;
static bool input_wake_ready = false;

static void input_wake_isr(void* arg) {
    (void)arg;
    /* Level triggered: mask every armed pin, or the ISR would fire again
     * right after returning while the button is still held. */
    for(size_t i = 0; i < input_wake_pin_count; i++) {
        gpio_intr_disable(input_wake_pins[i]);
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(input_wake_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

bool input_wake_init(const gpio_num_t* pins, size_t count) {
    if(input_wake_ready) return true;
    if(count == 0 || count > INPUT_WAKE_MAX_PINS) return false;

    input_wake_sem = xSemaphoreCreateBinaryStatic(&input_wake_sem_buffer);

    /* Normally installed by furi_hal_init() already. */
    const esp_err_t isr_err = gpio_install_isr_service(0);
    if(isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPIO ISR service unavailable: %s", esp_err_to_name(isr_err));
        return false;
    }

    for(size_t i = 0; i < count; i++) {
        input_wake_pins[i] = pins[i];
        gpio_intr_disable(pins[i]);
        if(gpio_isr_handler_add(pins[i], input_wake_isr, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%d wake handler failed; input keeps polling", pins[i]);
            for(size_t j = 0; j < i; j++) gpio_isr_handler_remove(pins[j]);
            return false;
        }
    }
    input_wake_pin_count = count;

    if(esp_sleep_enable_gpio_wakeup() != ESP_OK) {
        ESP_LOGE(TAG, "GPIO light-sleep wakeup unavailable; input keeps polling");
        return false;
    }

    input_wake_ready = true;
    return true;
}

bool input_wake_wait(const InputWakePin* pins, size_t count, uint32_t timeout_ms) {
    if(!input_wake_ready) return false;

    xSemaphoreTake(input_wake_sem, 0); /* drop a stale wake */
    for(size_t i = 0; i < count; i++) {
        gpio_wakeup_enable(pins[i].pin, pins[i].level);
        gpio_intr_enable(pins[i].pin);
    }

    xSemaphoreTake(input_wake_sem, pdMS_TO_TICKS(timeout_ms));

    for(size_t i = 0; i < count; i++) {
        gpio_intr_disable(pins[i].pin);
        /* Re-enables the pad's sleep selection (back to its sleep state). */
        gpio_wakeup_disable(pins[i].pin);
        /* gpio_wakeup_enable() wrote the level type straight into the pad. */
        gpio_set_intr_type(pins[i].pin, GPIO_INTR_DISABLE);
    }
    return true;
}

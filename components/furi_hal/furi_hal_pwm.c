/**
 * @file furi_hal_pwm.c
 * PWM HAL for ESP32: LEDC square wave on the external GPIO header pins.
 *
 * LEDC timer 0/channel 0 drive the backlight and timer 1/channel 1 the buzzer, so
 * this HAL owns timers 2-3 and channels 2-3: at most two pins can run at once,
 * each with its own frequency.
 */

#include "furi_hal_pwm.h"
#include "furi_hal_gpio.h"
#include "furi_hal_power.h"
#include "boards/board.h"
#include <furi.h>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <esp_private/esp_gpio_reserve.h>
#include <freertos/semphr.h>
#include <soc/soc.h>

#define TAG "FuriHalPwm"

#ifndef APB_CLK_FREQ
#define APB_CLK_FREQ (80 * 1000 * 1000)
#endif

#define PWM_PIN_COUNT  5
#define PWM_SLOT_COUNT 2
#define PWM_SPEED_MODE LEDC_LOW_SPEED_MODE

#ifdef BOARD_PIN_EXT_GPIO_1
#define PWM_PIN_1 BOARD_PIN_EXT_GPIO_1
#else
#define PWM_PIN_1 UINT16_MAX
#endif
#ifdef BOARD_PIN_EXT_GPIO_2
#define PWM_PIN_2 BOARD_PIN_EXT_GPIO_2
#else
#define PWM_PIN_2 UINT16_MAX
#endif
#ifdef BOARD_PIN_EXT_GPIO_3
#define PWM_PIN_3 BOARD_PIN_EXT_GPIO_3
#else
#define PWM_PIN_3 UINT16_MAX
#endif
#ifdef BOARD_PIN_EXT_GPIO_4
#define PWM_PIN_4 BOARD_PIN_EXT_GPIO_4
#else
#define PWM_PIN_4 UINT16_MAX
#endif
#ifdef BOARD_PIN_EXT_GPIO_5
#define PWM_PIN_5 BOARD_PIN_EXT_GPIO_5
#else
#define PWM_PIN_5 UINT16_MAX
#endif

static const GpioPin pwm_pins[PWM_PIN_COUNT] = {
    {.port = NULL, .pin = PWM_PIN_1},
    {.port = NULL, .pin = PWM_PIN_2},
    {.port = NULL, .pin = PWM_PIN_3},
    {.port = NULL, .pin = PWM_PIN_4},
    {.port = NULL, .pin = PWM_PIN_5},
};

/* Reservations compare the owner by pointer, so reserve and unreserve share this one. */
static const char* const pwm_owner = "PWM";

typedef struct {
    bool active;
    uint8_t pin_index;
    uint32_t freq;
    uint32_t duty_res;
    const ledc_timer_t timer;
    const ledc_channel_t channel;
} PwmSlot;

static PwmSlot pwm_slots[PWM_SLOT_COUNT] = {
    {.timer = LEDC_TIMER_2, .channel = LEDC_CHANNEL_2},
    {.timer = LEDC_TIMER_3, .channel = LEDC_CHANNEL_3},
};

static StaticSemaphore_t pwm_mutex_storage;
static SemaphoreHandle_t pwm_mutex = NULL;
static portMUX_TYPE pwm_init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t pwm_active_count = 0;

static void pwm_lock(void) {
    if(!pwm_mutex) {
        portENTER_CRITICAL(&pwm_init_lock);
        if(!pwm_mutex) pwm_mutex = xSemaphoreCreateMutexStatic(&pwm_mutex_storage);
        portEXIT_CRITICAL(&pwm_init_lock);
    }
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
}

static void pwm_unlock(void) {
    xSemaphoreGive(pwm_mutex);
}

/** @return index into pwm_pins, or -1 for ids that are not a usable header pin. */
static int pwm_pin_index(FuriHalPwmOutputId channel) {
    if(channel < FuriHalPwmOutputIdExtGpio1 || channel > FuriHalPwmOutputIdExtGpio5) return -1;
    const int index = channel - FuriHalPwmOutputIdExtGpio1;
    return pwm_pins[index].pin == UINT16_MAX ? -1 : index;
}

static PwmSlot* pwm_find_slot(int pin_index) {
    for(size_t i = 0; i < PWM_SLOT_COUNT; i++) {
        if(pwm_slots[i].active && pwm_slots[i].pin_index == pin_index) return &pwm_slots[i];
    }
    return NULL;
}

static PwmSlot* pwm_free_slot(void) {
    for(size_t i = 0; i < PWM_SLOT_COUNT; i++) {
        if(!pwm_slots[i].active) return &pwm_slots[i];
    }
    return NULL;
}

/* Sleeping stops the LEDC counter and the pin would freeze, so keep awake while running. */
static void pwm_active_changed(bool started) {
    if(started) {
        if(pwm_active_count++ == 0) furi_hal_power_light_sleep_inhibit_enter();
    } else {
        furi_check(pwm_active_count > 0);
        if(--pwm_active_count == 0) furi_hal_power_light_sleep_inhibit_exit();
    }
}

/* Duty 2^res is the full-high setting LEDC supports. */
static uint32_t pwm_duty_value(uint32_t duty_res, uint8_t duty_pct) {
    if(duty_pct > 100) duty_pct = 100;
    return (((uint32_t)duty_pct << duty_res) + 50) / 100;
}

static uint32_t pwm_clamp_freq(uint32_t freq) {
    if(freq < FURI_HAL_PWM_FREQ_MIN) return FURI_HAL_PWM_FREQ_MIN;
    if(freq > FURI_HAL_PWM_FREQ_MAX) return FURI_HAL_PWM_FREQ_MAX;
    return freq;
}

static esp_err_t pwm_configure_timer(PwmSlot* slot, uint32_t freq) {
    freq = pwm_clamp_freq(freq);

    /* Finest duty resolution the APB clock still divides down to this frequency. */
    const uint32_t duty_res = ledc_find_suitable_duty_resolution(APB_CLK_FREQ, freq);
    if(duty_res == 0) return ESP_ERR_INVALID_ARG;

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = PWM_SPEED_MODE,
        .timer_num = slot->timer,
        .duty_resolution = duty_res,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const esp_err_t err = ledc_timer_config(&timer_cfg);
    if(err == ESP_OK) {
        slot->freq = freq;
        slot->duty_res = duty_res;
    }
    return err;
}

/* Frequency and duty of a running slot. A new frequency can change the duty
 * resolution, so the duty is always written again after the timer. Reconfiguring
 * the timer restarts its counter (ledc_timer_config resets it), so a duty-only
 * change leaves the timer alone and the output keeps its phase. */
static esp_err_t pwm_apply(PwmSlot* slot, uint32_t freq, uint8_t duty_pct) {
    esp_err_t err = ESP_OK;
    if(pwm_clamp_freq(freq) != slot->freq) {
        err = pwm_configure_timer(slot, freq);
        if(err != ESP_OK) return err;
    }

    err = ledc_set_duty(PWM_SPEED_MODE, slot->channel, pwm_duty_value(slot->duty_res, duty_pct));
    if(err != ESP_OK) return err;
    return ledc_update_duty(PWM_SPEED_MODE, slot->channel);
}

/* Header pins idle as inputs pulled down, like the IR external TX pin. Only for
 * pins this HAL has reserved. gpio_reset_pin() also unroutes the LEDC signal;
 * LEDC marks the pad's output path as taken in IDF and never gives it back, so
 * do that here, otherwise the next RMT/LEDC user of the pad logs a bogus
 * "GPIO is not usable" warning. */
static void pwm_pin_set_idle(const GpioPin* pin) {
    const gpio_num_t gpio = (gpio_num_t)pin->pin;
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
    esp_gpio_revoke(BIT64(gpio));
}

static void pwm_slot_release(PwmSlot* slot) {
    const GpioPin* pin = &pwm_pins[slot->pin_index];
    ledc_stop(PWM_SPEED_MODE, slot->channel, 0);
    pwm_pin_set_idle(pin);
    slot->active = false;
    furi_hal_gpio_unreserve(pin, pwm_owner);
}

void furi_hal_pwm_start(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty) {
    const int pin_index = pwm_pin_index(channel);
    if(pin_index < 0) {
        FURI_LOG_W(TAG, "Output %d is not a header pin on this board", (int)channel);
        return;
    }
    const GpioPin* pin = &pwm_pins[pin_index];

    pwm_lock();
    PwmSlot* slot = pwm_find_slot(pin_index);
    if(slot) {
        /* Already running: behave like set_params. */
        const esp_err_t err = pwm_apply(slot, freq, duty);
        if(err != ESP_OK) FURI_LOG_E(TAG, "GPIO%u update failed: %s", pin->pin, esp_err_to_name(err));
        pwm_unlock();
        return;
    }

    slot = pwm_free_slot();
    if(!slot) {
        FURI_LOG_E(TAG, "No free LEDC timer for GPIO%u", pin->pin);
        pwm_unlock();
        return;
    }
    if(!furi_hal_gpio_reserve(pin, pwm_owner)) {
        FURI_LOG_E(
            TAG, "GPIO%u is in use by %s", pin->pin, furi_hal_gpio_get_reservation(pin));
        pwm_unlock();
        return;
    }
    if(furi_hal_gpio_is_output(pin)) {
        FURI_LOG_E(TAG, "GPIO%u is driven by another user", pin->pin);
        furi_hal_gpio_unreserve(pin, pwm_owner);
        pwm_unlock();
        return;
    }

    slot->pin_index = pin_index;
    esp_err_t err = pwm_configure_timer(slot, freq);
    if(err != ESP_OK) {
        /* The pad has not been touched yet: leave its configuration as it was. */
        FURI_LOG_E(TAG, "GPIO%u timer setup failed: %s", pin->pin, esp_err_to_name(err));
        furi_hal_gpio_unreserve(pin, pwm_owner);
        pwm_unlock();
        return;
    }

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = pin->pin,
        .speed_mode = PWM_SPEED_MODE,
        .channel = slot->channel,
        .timer_sel = slot->timer,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = pwm_duty_value(slot->duty_res, duty),
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if(err != ESP_OK) {
        /* The pad may already be routed to LEDC. */
        FURI_LOG_E(TAG, "GPIO%u channel setup failed: %s", pin->pin, esp_err_to_name(err));
        pwm_pin_set_idle(pin);
        furi_hal_gpio_unreserve(pin, pwm_owner);
        pwm_unlock();
        return;
    }

    slot->active = true;
    pwm_active_changed(true);
    pwm_unlock();
}

void furi_hal_pwm_stop(FuriHalPwmOutputId channel) {
    const int pin_index = pwm_pin_index(channel);
    if(pin_index < 0) return;

    pwm_lock();
    PwmSlot* slot = pwm_find_slot(pin_index);
    if(slot) {
        pwm_slot_release(slot);
        pwm_active_changed(false);
    }
    pwm_unlock();
}

void furi_hal_pwm_set_params(FuriHalPwmOutputId channel, uint32_t freq, uint8_t duty) {
    const int pin_index = pwm_pin_index(channel);
    if(pin_index < 0) return;

    pwm_lock();
    PwmSlot* slot = pwm_find_slot(pin_index);
    if(slot) {
        const esp_err_t err = pwm_apply(slot, freq, duty);
        if(err != ESP_OK) {
            FURI_LOG_E(
                TAG, "GPIO%u update failed: %s", pwm_pins[pin_index].pin, esp_err_to_name(err));
        }
    }
    pwm_unlock();
}

bool furi_hal_pwm_is_running(FuriHalPwmOutputId channel) {
    const int pin_index = pwm_pin_index(channel);
    if(pin_index < 0) return false;

    pwm_lock();
    const bool running = pwm_find_slot(pin_index) != NULL;
    pwm_unlock();
    return running;
}

/* High time in us as an LEDC duty, rounded to the nearest step; a pulse as long as the
 * period or longer is full-high (2^res). Past that check the product is below
 * 1e6 * 2^res, far from overflowing 64 bits. */
static uint32_t pwm_pulse_duty_value(uint32_t duty_res, uint32_t freq, uint32_t pulse_us) {
    const uint32_t full = 1UL << duty_res;
    if((uint64_t)pulse_us * freq >= 1000000ULL) return full;
    return (uint32_t)(((uint64_t)pulse_us * freq * full + 500000ULL) / 1000000ULL);
}

bool furi_hal_pwm_set_pulse(FuriHalPwmOutputId channel, uint32_t freq, uint32_t pulse_us) {
    const int pin_index = pwm_pin_index(channel);
    if(pin_index < 0) return false;

    pwm_lock();
    PwmSlot* slot = pwm_find_slot(pin_index);
    if(!slot) {
        pwm_unlock();
        return false;
    }

    /* Same rule as pwm_apply(): only a new frequency touches the timer (and its phase), so
     * a width change never cuts a pulse short or stretches it. The new duty is latched at
     * the next period. */
    esp_err_t err = ESP_OK;
    if(pwm_clamp_freq(freq) != slot->freq) err = pwm_configure_timer(slot, freq);
    if(err == ESP_OK) {
        err = ledc_set_duty(
            PWM_SPEED_MODE,
            slot->channel,
            pwm_pulse_duty_value(slot->duty_res, slot->freq, pulse_us));
    }
    if(err == ESP_OK) err = ledc_update_duty(PWM_SPEED_MODE, slot->channel);
    if(err != ESP_OK) {
        FURI_LOG_E(
            TAG, "GPIO%u pulse update failed: %s", pwm_pins[pin_index].pin, esp_err_to_name(err));
    }
    pwm_unlock();
    return err == ESP_OK;
}

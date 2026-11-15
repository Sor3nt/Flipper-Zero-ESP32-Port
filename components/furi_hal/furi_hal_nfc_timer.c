/**
 * @file furi_hal_nfc_timer.c
 * @brief NFC HAL timers for the ST25R3916 front-end (ESP-IDF port).
 *
 * The upstream implementation drives two STM32 general-purpose timers
 * (TIM1/TIM17) in one-pulse mode and signals expiry from their update
 * interrupts. ESP-IDF has no direct equivalent, so the same two timers are
 * emulated with 1 MHz general-purpose GPTimers: the alarm callback runs in
 * ISR context and only raises the corresponding internal NFC event, exactly
 * like the original update interrupt.
 *
 * Timer semantics preserved from upstream:
 *  - "fwt" — frame wait time timeout used by pollers.
 *  - "block_tx" — transmission block timeout (also used for frame delay).
 *  - both accept a duration in 13.56 MHz carrier cycles;
 *  - per-technology compensation is subtracted before programming the alarm.
 *
 * The GPTimer is a *level* peripheral, not a one-pulse one: when the alarm
 * fires it disables the alarm but keeps the driver in its RUN state and the
 * counter running. Combined with the way nfc.c arms the same timer again in
 * the same transaction (an RX-start event that is folded into RX-end skips
 * the stop), gptimer_start() then refuses with "timer is not ready for a new
 * start", the arming side ignored the error and the "running" flag stayed
 * set, so the next arm tripped furi_check(!running) and aborted the firmware
 * in the NFC worker thread. Arming therefore always brings the timer back to
 * the idle state first, and never trusts a failed start.
 *
 * The timers are created once and live for the whole session: low power
 * cycles used to delete and re-create them, which could race with the worker
 * thread, and an ESP32-S3 only has four GPTimers in total.
 */

#include "furi_hal_nfc_i.h"
#include "furi_hal_nfc_tech_i.h"

#include <driver/gptimer.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "FuriHalNfcTimer"

#define FURI_HAL_NFC_TIMER_RESOLUTION_HZ (1000000ULL)

typedef enum {
    FuriHalNfcTimerFwt,
    FuriHalNfcTimerBlockTx,
    FuriHalNfcTimerCount,
} FuriHalNfcTimer;

typedef struct {
    gptimer_handle_t timer;
    FuriHalNfcEventInternalType event;
    /** A timeout is armed as far as the callers are concerned. */
    volatile bool running;
    /** The counter may still be counting in hardware: an expired one-shot
     * alarm disables the alarm but leaves the driver in its RUN state. Used to
     * pre-stop the timer only when that is actually needed, so the driver does
     * not log "timer is not running" on every healthy re-arm. */
    volatile bool hw_running;
} FuriHalNfcTimerConfig;

static FuriHalNfcTimerConfig furi_hal_nfc_timers[FuriHalNfcTimerCount] = {
    [FuriHalNfcTimerFwt] =
        {
            .timer = NULL,
            .event = FuriHalNfcEventInternalTypeTimerFwtExpired,
            .running = false,
            .hw_running = false,
        },
    [FuriHalNfcTimerBlockTx] =
        {
            .timer = NULL,
            .event = FuriHalNfcEventInternalTypeTimerBlockTxExpired,
            .running = false,
            .hw_running = false,
        },
};

/* nfc_config()/low_power_mode_*() run on the app thread while the NFC worker
 * thread is using the timers, and the GPTimer state machine is not safe
 * against concurrent callers: gptimer_start()/gptimer_stop() compare-and-swap
 * through transient states, so the other thread can observe "not ready for a
 * new start" or "not running" and leave the timer unusable. Both timers are
 * only ever touched from task context, so a spinlock is enough and cannot
 * block. */
static portMUX_TYPE furi_hal_nfc_timer_lock = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR furi_hal_nfc_timer_irq_callback(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t* edata,
    void* context) {
    UNUSED(timer);
    UNUSED(edata);
    FuriHalNfcTimerConfig* config = context;
    config->running = false;
    furi_hal_nfc_event_set(config->event);
    return false;
}

static void furi_hal_nfc_timer_init(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];

    /* Called from every low_power_mode_stop() because the NFC app
     * reconfigures modes several times; a timer that is already live must be
     * reused instead of leaked. The ESP32-S3 only has four GPTimers. */
    if(config->timer) return;

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = FURI_HAL_NFC_TIMER_RESOLUTION_HZ,
    };
    esp_err_t err = gptimer_new_timer(&timer_config, &config->timer);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
        config->timer = NULL;
        return;
    }

    const gptimer_event_callbacks_t callbacks = {
        .on_alarm = furi_hal_nfc_timer_irq_callback,
    };
    err = gptimer_register_event_callbacks(config->timer, &callbacks, config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
        gptimer_del_timer(config->timer);
        config->timer = NULL;
        return;
    }

    err = gptimer_enable(config->timer);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "gptimer_enable failed: %s", esp_err_to_name(err));
        gptimer_del_timer(config->timer);
        config->timer = NULL;
        return;
    }

    FURI_LOG_D(TAG, "timer %d ready", (int)timer);
}

static void furi_hal_nfc_timer_deinit(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];

    portENTER_CRITICAL(&furi_hal_nfc_timer_lock);
    if(!config->timer) {
        portEXIT_CRITICAL(&furi_hal_nfc_timer_lock);
        return;
    }

    /* Both of these fail with ESP_ERR_INVALID_STATE when the alarm already
     * fired or the timer was never armed; both outcomes are expected here, so
     * only the delete has to succeed. */
    gptimer_stop(config->timer);
    gptimer_disable(config->timer);
    esp_err_t err = gptimer_del_timer(config->timer);
    if(err == ESP_OK) {
        config->timer = NULL;
    } else {
        /* Keep the handle so the next init can reuse it instead of leaking a
         * GPTimer; the timer stays disabled and unusable, which the arm path
         * reports. */
        FURI_LOG_E(TAG, "gptimer_del_timer failed: %s", esp_err_to_name(err));
    }
    config->running = false;
    portEXIT_CRITICAL(&furi_hal_nfc_timer_lock);
}

static int32_t furi_hal_nfc_timer_get_compensation(FuriHalNfcTimer timer) {
    const FuriHalNfcTechBase* current_tech = furi_hal_nfc_tech[furi_hal_nfc.tech];

    if(furi_hal_nfc.mode == FuriHalNfcModePoller) {
        const FuriHalNfcPollerCompensation* comp = &current_tech->poller.compensation;
        if(timer == FuriHalNfcTimerFwt)
            return comp->fwt;
        else if(timer == FuriHalNfcTimerBlockTx)
            return comp->fdt;

    } else if(furi_hal_nfc.mode == FuriHalNfcModeListener) {
        const FuriHalNfcListenerCompensation* comp = &current_tech->listener.compensation;
        if(timer == FuriHalNfcTimerBlockTx) return comp->fdt;
    }

    return 0;
}

static inline bool furi_hal_nfc_timer_is_running(FuriHalNfcTimer timer) {
    return furi_hal_nfc_timers[timer].running;
}

static esp_err_t furi_hal_nfc_timer_arm(FuriHalNfcTimerConfig* config, uint32_t time_us) {
    esp_err_t err;

    /* 0 would program an alarm in the past; fire on the next tick instead. */
    if(time_us == 0) time_us = 1;

    portENTER_CRITICAL(&furi_hal_nfc_timer_lock);

    /* A one-shot alarm does not stop the counter nor leave the driver idle, so
     * re-arming a timer whose previous timeout fired (or which was re-armed
     * before anybody stopped it) would be rejected by gptimer_start(). Only
     * pre-stop when that is possible, to keep the driver quiet. */
    if(config->hw_running) gptimer_stop(config->timer);

    gptimer_set_raw_count(config->timer, 0);

    const gptimer_alarm_config_t alarm_config = {
        .alarm_count = time_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    gptimer_set_alarm_action(config->timer, &alarm_config);

    /* Arm the flag *before* the start: a timeout that expires before this task
     * gets the spinlock back clears it from the ISR, and writing true
     * afterwards would resurrect an alarm that already fired, leaving callers
     * waiting for an event that was already consumed. */
    config->running = true;
    /* Assume the counter may be running even after a failure, so the next arm
     * forces the idle state instead of failing forever. */
    config->hw_running = true;

    err = gptimer_start(config->timer);
    if(err != ESP_OK) config->running = false;

    portEXIT_CRITICAL(&furi_hal_nfc_timer_lock);

    return err;
}

static void furi_hal_nfc_timer_start_tick(FuriHalNfcTimer timer, uint32_t time_us) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];
    if(!config->timer) return;

    esp_err_t err = furi_hal_nfc_timer_arm(config, time_us);
    if(err != ESP_OK) {
        /* Do not crash on a timer failure: the transaction above simply times
         * out and the poller retries. */
        FURI_LOG_D(TAG, "timer %d: arm failed: %s", (int)timer, esp_err_to_name(err));
    }
}

/** Convert carrier cycles to microseconds, rounding to nearest.
 *
 * us = fc / 13 560 000 * 1 000 000, i.e. fc * 1 000 000 / carrier. The
 * multiplier used to be 1000, which turns the result into milliseconds and
 * truncates every NFC timeout to 0 us, so the FWT/block-TX alarms fired on the
 * very next tick: every frame exchange ended in a timeout unless the card's
 * answer happened to be latched together with the TX-end interrupt. */
static uint32_t furi_hal_nfc_timer_fc_to_us(uint32_t time_fc) {
    return (uint32_t)(((uint64_t)time_fc * 1000000ULL + (FURI_HAL_NFC_CARRIER_HZ / 2U)) /
                      FURI_HAL_NFC_CARRIER_HZ);
}

static void furi_hal_nfc_timer_start_fc(FuriHalNfcTimer timer, uint32_t time_fc) {
    const int32_t comp_fc = furi_hal_nfc_timer_get_compensation(timer);
    /* Not starting the timer if the compensation value is greater than the
     * requested delay. */
    if(comp_fc >= (int32_t)time_fc) return;

    furi_hal_nfc_timer_start_tick(timer, furi_hal_nfc_timer_fc_to_us(time_fc - comp_fc));
}

static void furi_hal_nfc_timer_stop(FuriHalNfcTimer timer) {
    FuriHalNfcTimerConfig* config = &furi_hal_nfc_timers[timer];
    if(!config->timer) return;

    portENTER_CRITICAL(&furi_hal_nfc_timer_lock);
    /* ESP_ERR_INVALID_STATE here just means the alarm already fired or the
     * caller stopped a timer it had not started (nfc.c does both), so only
     * touch the hardware when a timeout is actually armed: that keeps the
     * driver from logging "timer is not running" on every power-down. When the
     * alarm already fired the counter is left running, and the next arm stops
     * it through hw_running. */
    if(config->running) {
        gptimer_stop(config->timer);
        gptimer_set_alarm_action(config->timer, NULL);
        config->hw_running = false;
    }
    config->running = false;
    portEXIT_CRITICAL(&furi_hal_nfc_timer_lock);
}

void furi_hal_nfc_timers_init(void) {
    for(size_t i = 0; i < FuriHalNfcTimerCount; i++) {
        furi_hal_nfc_timer_init(i);
    }
}

void furi_hal_nfc_timers_deinit(void) {
    for(size_t i = 0; i < FuriHalNfcTimerCount; i++) {
        furi_hal_nfc_timer_deinit(i);
    }
}

void furi_hal_nfc_timers_stop(void) {
    for(size_t i = 0; i < FuriHalNfcTimerCount; i++) {
        furi_hal_nfc_timer_stop(i);
    }
}

void furi_hal_nfc_timer_fwt_start(uint32_t time_fc) {
    furi_hal_nfc_timer_start_fc(FuriHalNfcTimerFwt, time_fc);
}

void furi_hal_nfc_timer_fwt_stop(void) {
    furi_hal_nfc_timer_stop(FuriHalNfcTimerFwt);
}

void furi_hal_nfc_timer_block_tx_start(uint32_t time_fc) {
    furi_hal_nfc_timer_start_fc(FuriHalNfcTimerBlockTx, time_fc);
}

void furi_hal_nfc_timer_block_tx_start_us(uint32_t time_us) {
    furi_hal_nfc_timer_start_tick(FuriHalNfcTimerBlockTx, time_us);
}

void furi_hal_nfc_timer_block_tx_stop(void) {
    furi_hal_nfc_timer_stop(FuriHalNfcTimerBlockTx);
}

bool furi_hal_nfc_timer_block_tx_is_running(void) {
    return furi_hal_nfc_timer_is_running(FuriHalNfcTimerBlockTx);
}
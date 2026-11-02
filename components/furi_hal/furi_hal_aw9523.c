#include "furi_hal_aw9523.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <furi.h>

#include "boards/board.h"

#if defined(BOARD_HAS_AW9523) && BOARD_HAS_AW9523

#define TAG "FuriHalAw9523"
#define AW9523_REG_OUTPUT0 0x02
#define AW9523_REG_OUTPUT1 0x03
#define AW9523_REG_CONFIG0 0x04
#define AW9523_REG_CONFIG1 0x05
#define AW9523_REG_CHIP_ID 0x10
#define AW9523_REG_GCR     0x11
#define AW9523_REG_LEDMODE0 0x12
#define AW9523_REG_LEDMODE1 0x13
#define AW9523_P0_7_MASK 0x80
#define AW9523_P1_4_MASK 0x10
#define AW9523_TIMEOUT pdMS_TO_TICKS(20)
#define AW9523_RETRIES 3
#define AW9523_RETRY_COOLDOWN_US 1000000LL

static bool aw9523_present;
static bool aw9523_initialized;
static uint8_t aw9523_output0;
static uint8_t aw9523_output1;
/* aw9523_output0/1 mirror the chip only while this is true. A failed multi-register
 * update leaves the chip in a state the cache does not describe; the next request must
 * then be written even if it equals the cached value, or the RF path would stay off
 * (or half set) while every caller believes it is switched. */
static bool aw9523_outputs_known;
static FuriMutex* aw9523_mutex;
static int64_t aw9523_retry_after_us;
static uint8_t aw9523_logged_state = 0xFF;

static bool aw9523_write_register(uint8_t reg, uint8_t value) {
#if defined(BOARD_HAS_AW9523) && BOARD_HAS_AW9523
    const uint8_t data[] = {reg, value};
    esp_err_t err = ESP_FAIL;
    for(uint32_t attempt = 0; attempt < AW9523_RETRIES; attempt++) {
        err = i2c_master_write_to_device(
            I2C_NUM_0, BOARD_AW9523_I2C_ADDR, data, sizeof(data), AW9523_TIMEOUT);
        if(err == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGW(TAG, "AW9523 write reg 0x%02X=0x%02X failed: %s", reg, value, esp_err_to_name(err));
    return false;
#else
    UNUSED(reg);
    UNUSED(value);
    return false;
#endif
}

static bool aw9523_read_register(uint8_t reg, uint8_t* value) {
#if defined(BOARD_HAS_AW9523) && BOARD_HAS_AW9523
    esp_err_t err = i2c_master_write_read_device(
        I2C_NUM_0, BOARD_AW9523_I2C_ADDR, &reg, 1, value, 1, AW9523_TIMEOUT);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "AW9523 read reg 0x%02X failed: %s", reg, esp_err_to_name(err));
        return false;
    }
    return true;
#else
    UNUSED(reg);
    UNUSED(value);
    return false;
#endif
}

static bool aw9523_apply_outputs(uint8_t output0, uint8_t output1) {
    /*
     * Always break the RF path before selecting the next path. The output
     * registers are written independently, so writing OUTPUT0 first could
     * briefly leave RXEN and TXEN high at the same time during TX -> RX.
     */
    const uint8_t safe_output0 = output0 & (uint8_t)~AW9523_P0_7_MASK;
    const uint8_t safe_output1 = output1 & (uint8_t)~AW9523_P1_4_MASK;
    aw9523_outputs_known = false; /* until every write below and the read-back succeed */
    if(!aw9523_write_register(AW9523_REG_OUTPUT1, safe_output1)) return false;
    if(!aw9523_write_register(AW9523_REG_OUTPUT0, safe_output0)) return false;

    if(output1 != safe_output1 && !aw9523_write_register(AW9523_REG_OUTPUT1, output1)) {
        return false;
    }
    if(output0 != safe_output0 && !aw9523_write_register(AW9523_REG_OUTPUT0, output0)) {
        return false;
    }

    uint8_t actual0 = 0;
    uint8_t actual1 = 0;
    if(!aw9523_read_register(AW9523_REG_OUTPUT0, &actual0) ||
       !aw9523_read_register(AW9523_REG_OUTPUT1, &actual1)) {
        return false;
    }
    const uint8_t controlled_mask0 = AW9523_P0_7_MASK;
    const uint8_t controlled_mask1 = AW9523_P1_4_MASK;
    if((actual0 & controlled_mask0) != (output0 & controlled_mask0) ||
       (actual1 & controlled_mask1) != (output1 & controlled_mask1)) {
        ESP_LOGW(
            TAG,
            "AW9523 output verify mismatch: wanted RXEN=%d TXEN=%d got RXEN=%d TXEN=%d",
            (output0 & controlled_mask0) != 0,
            (output1 & controlled_mask1) != 0,
            (actual0 & controlled_mask0) != 0,
            (actual1 & controlled_mask1) != 0);
        return false;
    }

    aw9523_output0 = output0;
    aw9523_output1 = output1;
    aw9523_outputs_known = true;
    ESP_LOGD(
        TAG,
        "RF front-end state verified: RXEN=%s (P0_7) TXEN=%s (P1_4)",
        (actual0 & controlled_mask0) ? "HIGH" : "LOW",
        (actual1 & controlled_mask1) ? "HIGH" : "LOW");
    return true;
}

/* The first caller may be any radio thread, so two of them can race here: install the
 * mutex with a compare-and-set instead of a plain check-then-assign, otherwise each
 * would lock its own mutex and the I2C sequences would not exclude each other. */
static void aw9523_ensure_mutex(void) {
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    if(aw9523_mutex) return;

    FuriMutex* candidate = furi_mutex_alloc(FuriMutexTypeRecursive);
    furi_check(candidate);
    portENTER_CRITICAL(&lock);
    if(!aw9523_mutex) aw9523_mutex = candidate;
    FuriMutex* installed = aw9523_mutex;
    portEXIT_CRITICAL(&lock);
    if(installed != candidate) furi_mutex_free(candidate);
}

void furi_hal_aw9523_init(void) {
#if defined(BOARD_HAS_AW9523) && BOARD_HAS_AW9523
    aw9523_ensure_mutex();
    furi_check(furi_mutex_acquire(aw9523_mutex, FuriWaitForever) == FuriStatusOk);
    if(aw9523_initialized) {
        furi_check(furi_mutex_release(aw9523_mutex) == FuriStatusOk);
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if(now_us < aw9523_retry_after_us) {
        furi_check(furi_mutex_release(aw9523_mutex) == FuriStatusOk);
        return;
    }

    uint8_t chip_id = 0;
    if(!aw9523_read_register(AW9523_REG_CHIP_ID, &chip_id) || chip_id != 0x23) {
        aw9523_present = false;
        aw9523_initialized = false;
        aw9523_retry_after_us = now_us + AW9523_RETRY_COOLDOWN_US;
        ESP_LOGW(
            TAG,
            "AW9523 probe failed at 0x%02X (chip_id=0x%02X)",
            BOARD_AW9523_I2C_ADDR,
            chip_id);
        furi_check(furi_mutex_release(aw9523_mutex) == FuriStatusOk);
        return;
    }

    /* Set safe low latches before switching P0_7/P1_4 to push-pull outputs. */
    const bool ok = aw9523_write_register(AW9523_REG_LEDMODE0, 0xFF) &&
                    aw9523_write_register(AW9523_REG_LEDMODE1, 0xFF) &&
                    aw9523_write_register(AW9523_REG_GCR, 0x10) &&
                    aw9523_apply_outputs(0x00, 0x00) &&
                    aw9523_write_register(AW9523_REG_CONFIG0, (uint8_t)~AW9523_P0_7_MASK) &&
                    aw9523_write_register(AW9523_REG_CONFIG1, (uint8_t)~AW9523_P1_4_MASK);
    aw9523_present = ok;
    /* Keep initialization retryable after a transient I2C failure. */
    aw9523_initialized = ok;
    if(!ok) {
        aw9523_retry_after_us = now_us + AW9523_RETRY_COOLDOWN_US;
    }
    if(ok) {
        ESP_LOGI(TAG, "AW9523 ready at 0x%02X (RXEN=P0_7, TXEN=P1_4)", BOARD_AW9523_I2C_ADDR);
    } else {
        ESP_LOGW(TAG, "AW9523 not detected at 0x%02X; RF front-end remains disabled", BOARD_AW9523_I2C_ADDR);
    }
    furi_check(furi_mutex_release(aw9523_mutex) == FuriStatusOk);
#else
    aw9523_initialized = true;
#endif
}

static bool aw9523_set_outputs(uint8_t output0, uint8_t output1) {
#if !defined(BOARD_HAS_AW9523) || !BOARD_HAS_AW9523
    UNUSED(output0);
    UNUSED(output1);
    return true;
#else
    furi_hal_aw9523_init();
    if(aw9523_mutex) {
        furi_check(furi_mutex_acquire(aw9523_mutex, FuriWaitForever) == FuriStatusOk);
    }
    bool ok = aw9523_present;
    if(aw9523_present &&
       (!aw9523_outputs_known || aw9523_output0 != output0 || aw9523_output1 != output1) &&
       !aw9523_apply_outputs(output0, output1)) {
        ESP_LOGW(TAG, "AW9523 output update failed; RF front-end state may be stale");
        ok = false;
    }
    if(aw9523_mutex) {
        furi_check(furi_mutex_release(aw9523_mutex) == FuriStatusOk);
    }
    return ok;
#endif
}

bool furi_hal_aw9523_set_idle(void) {
    const bool ok = aw9523_set_outputs(
        aw9523_output0 & (uint8_t)~AW9523_P0_7_MASK,
        aw9523_output1 & (uint8_t)~AW9523_P1_4_MASK);
    if(ok && aw9523_logged_state != 0) {
        ESP_LOGI(TAG, "RF front-end IDLE: RXEN=LOW TXEN=LOW");
        aw9523_logged_state = 0;
    }
    return ok;
}

bool furi_hal_aw9523_set_rx(void) {
    /* Disable TX before enabling RX; never assert both front-end paths. */
    const bool ok = aw9523_set_outputs(
        aw9523_output0 | AW9523_P0_7_MASK,
        aw9523_output1 & (uint8_t)~AW9523_P1_4_MASK);
    if(ok && aw9523_logged_state != 1) {
        ESP_LOGI(TAG, "RF front-end RX active: RXEN=HIGH TXEN=LOW");
        aw9523_logged_state = 1;
    }
    return ok;
}

bool furi_hal_aw9523_set_tx(void) {
    /* Disable RX before enabling TX; never assert both front-end paths. */
    const bool ok = aw9523_set_outputs(
        aw9523_output0 & (uint8_t)~AW9523_P0_7_MASK,
        aw9523_output1 | AW9523_P1_4_MASK);
    if(ok && aw9523_logged_state != 2) {
        ESP_LOGI(TAG, "RF front-end TX active: RXEN=LOW TXEN=HIGH");
        aw9523_logged_state = 2;
    }
    return ok;
}

bool furi_hal_aw9523_is_present(void) {
    furi_hal_aw9523_init();
    return aw9523_present;
}

#else

void furi_hal_aw9523_init(void) {
}

bool furi_hal_aw9523_set_idle(void) {
    return true;
}

bool furi_hal_aw9523_set_rx(void) {
    return true;
}

bool furi_hal_aw9523_set_tx(void) {
    return true;
}

bool furi_hal_aw9523_is_present(void) {
    return false;
}

#endif

/**
 * @file furi_hal_touch.c
 * CST816S capacitive touch controller driver (I2C)
 *
 * Configuration driven by boards/board.h
 * Uses legacy i2c driver API (CST816S doesn't work with new i2c_master API).
 */

#include "furi_hal_touch.h"
#include "boards/board.h"

#include <furi.h>
#include <esp_log.h>
#include <driver/i2c.h>

#define TAG "FuriHalTouch"

/* Hardware configuration from board header */
#define TOUCH_I2C_ADDR     BOARD_TOUCH_I2C_ADDR
#define TOUCH_SCL_PIN      BOARD_PIN_TOUCH_SCL
#define TOUCH_SDA_PIN      BOARD_PIN_TOUCH_SDA
#define TOUCH_I2C_PORT     BOARD_TOUCH_I2C_PORT
#define TOUCH_I2C_FREQ_HZ  BOARD_TOUCH_I2C_FREQ_HZ
#define TOUCH_I2C_TIMEOUT  BOARD_TOUCH_I2C_TIMEOUT

/* State */
static bool touch_initialized = false;
static volatile FuriThreadId touch_notify_thread = NULL;

static bool touch_i2c_read(uint8_t reg, uint8_t* data, size_t len) {
    esp_err_t err = i2c_master_write_read_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, data, len, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
}

static bool touch_i2c_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_write_to_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, buf, 2, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
}

void furi_hal_touch_init(void) {
    ESP_LOGI(TAG, "Initializing CST816S touch controller");

    /* Initialize I2C bus (legacy driver, same as Waveshare demo) */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = TOUCH_SCL_PIN,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(TOUCH_I2C_PORT, &conf);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return;
    }

    err = i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return;
    }

    /* Switch to normal mode (same as Waveshare demo: write 0x00 to reg 0x00) */
    if(touch_i2c_write(0x00, 0x00)) {
        ESP_LOGI(TAG, "CST816S set to normal mode");
    } else {
        ESP_LOGW(TAG, "CST816S not responding (may wake on first touch)");
    }

    /* Disable auto-sleep so chip stays responsive to I2C polling */
    if(touch_i2c_write(0xFE, 0x01)) {
        ESP_LOGI(TAG, "CST816S auto-sleep disabled");
    } else {
        ESP_LOGW(TAG, "CST816S auto-sleep disable failed");
    }

    /* Try to read chip ID */
    uint8_t chip_id = 0;
    if(touch_i2c_read(0xA7, &chip_id, 1)) {
        ESP_LOGI(TAG, "CST816S chip ID: 0x%02X", chip_id);
    }

    touch_initialized = true;
    ESP_LOGI(TAG, "Touch init OK (I2C polling mode)");
}

bool furi_hal_touch_is_pressed(void) {
    if(!touch_initialized) return false;

    uint8_t data[7] = {0};
    if(!touch_i2c_read(0x00, data, 7)) {
        return false;
    }
    /* data[2] = finger count (0 or 1) */
    return (data[2] > 0);
}

void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y) {
    if(!touch_initialized) {
        *x = 0;
        *y = 0;
        return;
    }

    uint8_t data[7] = {0};
    if(!touch_i2c_read(0x00, data, 7)) {
        *x = 0;
        *y = 0;
        return;
    }

    /* Same decoding as Waveshare demo */
    *x = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
    *y = ((uint16_t)(data[5] & 0x0F) << 8) | data[6];
}

bool furi_hal_touch_read(TouchData* data) {
    if(!touch_initialized || !data) {
        if(data) {
            data->gesture = TouchGestureNone;
            data->finger_count = 0;
            data->x = 0;
            data->y = 0;
        }
        return false;
    }

    uint8_t raw[7] = {0};
    if(!touch_i2c_read(0x00, raw, 7)) {
        data->gesture = TouchGestureNone;
        data->finger_count = 0;
        data->x = 0;
        data->y = 0;
        return false;
    }

    data->gesture = (TouchGesture)raw[1];
    data->finger_count = raw[2];
    data->x = ((uint16_t)(raw[3] & 0x0F) << 8) | raw[4];
    data->y = ((uint16_t)(raw[5] & 0x0F) << 8) | raw[6];
    return true;
}

uint8_t furi_hal_touch_get_gesture(void) {
    if(!touch_initialized) return 0;

    uint8_t data[7] = {0};
    if(!touch_i2c_read(0x00, data, 7)) return 0;
    return data[1]; /* gesture ID */
}

bool furi_hal_touch_int_active(void) {
    /* No INT pin used — always return false, use polling instead */
    return false;
}

void furi_hal_touch_set_notify_thread(void* thread_id) {
    touch_notify_thread = (FuriThreadId)thread_id;
}

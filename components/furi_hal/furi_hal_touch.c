/**
 * @file furi_hal_touch.c
 * Capacitive touch controller driver (I2C, legacy driver API).
 *
 * Supports two controllers, selected by the board header:
 *   - CST816S  (default): e.g. Waveshare C6-LCD-1.9    @ 0x15, data reg 0x00
 *   - AXS5106L (#define BOARD_TOUCH_AXS5106L): e.g.
 *                          Waveshare C6-Touch-LCD-1.47  @ 0x63, data reg 0x01
 *
 * Both report touch points as 12-bit X/Y with the high nibble in the first
 * byte (& 0x0F) — only the data register, packet length and field offsets
 * differ, so they share one decode path parameterised by the macros below.
 *
 * Uses the legacy i2c driver API (these chips don't work with i2c_master.h).
 */

#include "furi_hal_touch.h"
#include "boards/board.h"

#include <furi.h>
#include <esp_log.h>
#include <driver/i2c.h>
#include <driver/gpio.h>

#define TAG "FuriHalTouch"

/* Hardware configuration from board header */
#define TOUCH_I2C_ADDR     BOARD_TOUCH_I2C_ADDR
#define TOUCH_SCL_PIN      BOARD_PIN_TOUCH_SCL
#define TOUCH_SDA_PIN      BOARD_PIN_TOUCH_SDA
#define TOUCH_I2C_PORT     BOARD_TOUCH_I2C_PORT
#define TOUCH_I2C_FREQ_HZ  BOARD_TOUCH_I2C_FREQ_HZ
#define TOUCH_I2C_TIMEOUT  BOARD_TOUCH_I2C_TIMEOUT

/* ---- Controller-specific data layout --------------------------------------
 * raw[FINGER_IDX]            = finger count
 * raw[XY_OFFSET + 0/1]       = X high(&0x0F)/low byte
 * raw[XY_OFFSET + 2/3]       = Y high(&0x0F)/low byte
 * RST_LOW/HIGH_MS            = reset pulse timing for this chip
 */
#ifdef BOARD_TOUCH_AXS5106L
#define TOUCH_CHIP_NAME    "AXS5106L"
#define TOUCH_DATA_REG     0x01
#define TOUCH_DATA_LEN     14
#define TOUCH_FINGER_IDX   1
#define TOUCH_XY_OFFSET    2
#define TOUCH_RST_LOW_MS   200
#define TOUCH_RST_HIGH_MS  300
#else
#define TOUCH_CHIP_NAME    "CST816S"
#define TOUCH_DATA_REG     0x00
#define TOUCH_DATA_LEN     7
#define TOUCH_FINGER_IDX   2
#define TOUCH_XY_OFFSET    3
#define TOUCH_RST_LOW_MS   10
#define TOUCH_RST_HIGH_MS  50
#endif

/* State */
static bool touch_initialized = false;
static volatile FuriThreadId touch_notify_thread = NULL;

static bool touch_i2c_read(uint8_t reg, uint8_t* data, size_t len) {
#ifdef BOARD_TOUCH_AXS5106L
    /* The AXS5106L does NOT support I2C repeated-start: it needs a STOP between
     * the register-address write and the read (write reg + STOP, then a fresh
     * read transaction). Using write_read_device (repeated-start) makes the
     * chip ACK the address probe but never return register data. */
    if(i2c_master_write_to_device(
           TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, TOUCH_I2C_TIMEOUT) != ESP_OK) {
        return false;
    }
    return i2c_master_read_from_device(
               TOUCH_I2C_PORT, TOUCH_I2C_ADDR, data, len, TOUCH_I2C_TIMEOUT) == ESP_OK;
#else
    esp_err_t err = i2c_master_write_read_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, data, len, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
#endif
}

#ifndef BOARD_TOUCH_AXS5106L
static bool touch_i2c_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_write_to_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, buf, 2, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
}
#endif

/* Read the controller's touch packet and decode finger count + first point. */
static bool touch_read_point(uint8_t* finger_count, uint16_t* x, uint16_t* y) {
    uint8_t raw[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, raw, TOUCH_DATA_LEN)) {
        return false;
    }
    *finger_count = raw[TOUCH_FINGER_IDX];
    *x = ((uint16_t)(raw[TOUCH_XY_OFFSET + 0] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 1];
    *y = ((uint16_t)(raw[TOUCH_XY_OFFSET + 2] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 3];
    return true;
}

void furi_hal_touch_init(void) {
    ESP_LOGI(TAG, "Initializing %s touch controller", TOUCH_CHIP_NAME);

    /* Pulse the touch controller's reset line before talking to it. The
     * Waveshare C6-LCD boards hold the controller in reset unless the host
     * drives TP_RST high — without this the chip never ACKs on I2C. Active-low
     * reset: low for RST_LOW_MS, then high, then RST_HIGH_MS for the chip to
     * boot (the AXS5106L needs ~200/300ms, the CST816S much less). Boards with
     * no TP_RST pin (or a hardware pull-up, e.g. the 1.9) are unaffected. */
#ifdef BOARD_PIN_TOUCH_RST
    gpio_set_direction((gpio_num_t)BOARD_PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_TOUCH_RST, 0);
    furi_delay_ms(TOUCH_RST_LOW_MS);
    gpio_set_level((gpio_num_t)BOARD_PIN_TOUCH_RST, 1);
    furi_delay_ms(TOUCH_RST_HIGH_MS);
#endif

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

#ifdef BOARD_TOUCH_AXS5106L
    /* AXS5106L: there is no "normal mode" command like the CST816S; just verify
     * the chip answers by reading its device ID from register 0x08. */
    uint8_t id[2] = {0};
    if(touch_i2c_read(0x08, id, 2)) {
        ESP_LOGI(TAG, "AXS5106L device ID: 0x%02X%02X", id[0], id[1]);
    } else {
        ESP_LOGW(TAG, "AXS5106L not responding (addr 0x%02X)", TOUCH_I2C_ADDR);
    }
#else
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
#endif

    touch_initialized = true;
    ESP_LOGI(TAG, "Touch init OK (I2C polling mode)");
}

bool furi_hal_touch_is_pressed(void) {
    if(!touch_initialized) return false;

    uint8_t finger_count = 0;
    uint16_t x = 0, y = 0;
    if(!touch_read_point(&finger_count, &x, &y)) {
        return false;
    }
    return (finger_count > 0);
}

void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y) {
    *x = 0;
    *y = 0;
    if(!touch_initialized) return;

    uint8_t finger_count = 0;
    touch_read_point(&finger_count, x, y);
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

    uint8_t raw[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, raw, TOUCH_DATA_LEN)) {
        data->gesture = TouchGestureNone;
        data->finger_count = 0;
        data->x = 0;
        data->y = 0;
        return false;
    }

#ifdef BOARD_TOUCH_AXS5106L
    /* The AXS5106L doesn't report hardware gestures — leave gesture None and
     * let the input layer derive swipes from X/Y motion. */
    data->gesture = TouchGestureNone;
#else
    data->gesture = (TouchGesture)raw[1];
#endif
    data->finger_count = raw[TOUCH_FINGER_IDX];
    data->x = ((uint16_t)(raw[TOUCH_XY_OFFSET + 0] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 1];
    data->y = ((uint16_t)(raw[TOUCH_XY_OFFSET + 2] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 3];
    return true;
}

uint8_t furi_hal_touch_get_gesture(void) {
    if(!touch_initialized) return 0;

#ifdef BOARD_TOUCH_AXS5106L
    return 0; /* no hardware gestures on this controller */
#else
    uint8_t data[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, data, TOUCH_DATA_LEN)) return 0;
    return data[1]; /* gesture ID */
#endif
}

bool furi_hal_touch_int_active(void) {
    /* No INT pin used — always return false, use polling instead */
    return false;
}

void furi_hal_touch_set_notify_thread(void* thread_id) {
    touch_notify_thread = (FuriThreadId)thread_id;
}

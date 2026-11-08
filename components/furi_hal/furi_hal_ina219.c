/**
 * @file furi_hal_ina219.c
 * INA219 current/voltage/power monitor driver — ESP-IDF legacy I2C API.
 *
 * Board wiring (Fatal Core): INA219 at 0x40 on the shared I2C bus
 * (SDA=GPIO47, SCL=GPIO48) measuring the battery rail behind the TP4056.
 *
 * Unlike a coulomb-counting fuel gauge the INA219 only reports instantaneous
 * voltage/current, so charge percentage is derived from the bus voltage by
 * furi_hal_power.c using its battery discharge curve.
 */

#include "furi_hal_ina219.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "INA219";

#ifndef BOARD_INA219_ADDR
#define BOARD_INA219_ADDR 0x40
#endif

/* Shunt resistor on the board, in milliohms. Fatal Core uses a 50 mΩ (1206)
 * shunt (see the vendor BOM); most INA219 breakouts use 0.1 Ω, hence the
 * generic default below. */
#ifndef BOARD_INA219_SHUNT_MOHM
#define BOARD_INA219_SHUNT_MOHM 100
#endif

/* Full-scale current used for calibration. Must be >= the largest current the
 * rail can draw, otherwise the current register clips. */
#ifndef BOARD_INA219_MAX_CURRENT_MA
#define BOARD_INA219_MAX_CURRENT_MA 3200
#endif

#ifdef BOARD_I2C_PORT
#define INA_I2C_PORT BOARD_I2C_PORT
#else
#define INA_I2C_PORT I2C_NUM_0
#endif

#define INA_I2C_FREQ_HZ 100000
#define INA_I2C_TIMEOUT (1000 / portTICK_PERIOD_MS)

/* Registers */
#define INA219_REG_CONFIG    0x00
#define INA219_REG_SHUNT     0x01
#define INA219_REG_BUS       0x02
#define INA219_REG_POWER     0x03
#define INA219_REG_CURRENT   0x04
#define INA219_REG_CALIB     0x05

/* CONFIG: 32 V range, +/-320 mV shunt PGA, 12-bit ADC, 32-sample averaging
 * (8.51 ms conversion each), continuous shunt+bus. */
#define INA219_CONFIG_VALUE 0x399F

static bool ina_initialized = false;
static bool ina_present = false;

/* Current LSB in microamps, derived from the calibration. */
static uint32_t ina_current_lsb_ua = 100;

static bool ina_i2c_write16(uint8_t reg, uint16_t value) {
    uint8_t data[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_INA219_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, data, sizeof(data), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(INA_I2C_PORT, cmd, INA_I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool ina_i2c_read16(uint8_t reg, uint16_t* value) {
    uint8_t data[2] = {0};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_INA219_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_INA219_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(INA_I2C_PORT, cmd, INA_I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);
    if(err != ESP_OK) return false;

    *value = ((uint16_t)data[0] << 8) | data[1];
    return true;
}

bool furi_hal_ina219_init(void) {
    if(ina_initialized) return ina_present;
    ina_initialized = true;

    /* I2C bus is installed by furi_hal_power_init() before this is called. */

    /* Probe: a configuration write is harmless and confirms an ACK. */
    if(!ina_i2c_write16(INA219_REG_CONFIG, INA219_CONFIG_VALUE)) {
        ESP_LOGW(TAG, "INA219 not found at 0x%02X (no ACK)", BOARD_INA219_ADDR);
        ina_present = false;
        return false;
    }

    /* Calibration: current_lsb = max_current / 32768
     * cal = trunc(0.04096 / (current_lsb[A] * r_shunt[ohm])) */
    const uint32_t current_lsb_ua =
        ((uint32_t)BOARD_INA219_MAX_CURRENT_MA * 1000U) / 32768U;
    const uint32_t cal = (uint32_t)(0.04096f /
        (((float)current_lsb_ua / 1000000.0f) * ((float)BOARD_INA219_SHUNT_MOHM / 1000.0f)));

    if(cal == 0 || cal > 0xFFFF) {
        ESP_LOGE(TAG, "INA219 calibration out of range (cal=%u)", (unsigned)cal);
        ina_present = false;
        return false;
    }

    if(!ina_i2c_write16(INA219_REG_CALIB, (uint16_t)cal)) {
        ESP_LOGE(TAG, "INA219 calibration write failed");
        ina_present = false;
        return false;
    }

    ina_current_lsb_ua = current_lsb_ua ? current_lsb_ua : 1;
    ina_present = true;

    ESP_LOGI(
        TAG,
        "INA219 ready (addr=0x%02X, shunt=%dmOhm, cal=%u, lsb=%uuA)",
        BOARD_INA219_ADDR,
        BOARD_INA219_SHUNT_MOHM,
        (unsigned)cal,
        (unsigned)ina_current_lsb_ua);
    return true;
}

bool furi_hal_ina219_is_present(void) {
    return ina_present;
}

uint16_t furi_hal_ina219_get_voltage_mv(void) {
    uint16_t raw = 0;
    if(!ina_present || !ina_i2c_read16(INA219_REG_BUS, &raw)) return 0;
    /* Bits 15..3 hold the bus voltage, 4 mV per LSB. Bit 1 is the math
     * overflow flag; masked off with the shift below. */
    return (uint16_t)((raw >> 3) * 4U);
}

int16_t furi_hal_ina219_get_current_ma(void) {
    uint16_t raw = 0;
    if(!ina_present || !ina_i2c_read16(INA219_REG_CURRENT, &raw)) return 0;
    const int32_t ua = (int32_t)(int16_t)raw * (int32_t)ina_current_lsb_ua;
    return (int16_t)(ua / 1000);
}

uint32_t furi_hal_ina219_get_power_mw(void) {
    uint16_t raw = 0;
    if(!ina_present || !ina_i2c_read16(INA219_REG_POWER, &raw)) return 0;
    /* Power LSB = 20 * current LSB. */
    const uint64_t uw = (uint64_t)raw * 20U * ina_current_lsb_ua;
    return (uint32_t)(uw / 1000U);
}

int32_t furi_hal_ina219_get_shunt_uv(void) {
    uint16_t raw = 0;
    if(!ina_present || !ina_i2c_read16(INA219_REG_SHUNT, &raw)) return 0;
    /* 10 uV per LSB, signed. */
    return (int32_t)(int16_t)raw * 10;
}
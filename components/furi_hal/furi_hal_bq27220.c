/**
 * @file furi_hal_bq27220.c
 * BQ27220 fuel gauge driver — ESP-IDF I2C (legacy driver API)
 * API mirrors STM32 Flipper firmware (lib/drivers/bq27220.c).
 *
 * I2C address: 0x55
 * Bus: Qwiic/Grove (SDA=GPIO8, SCL=GPIO18 on T-Embed CC1101)
 */

#include "furi_hal_bq27220.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "BQ27220";

/* Registers */
#define BQ27220_REG_TEMP        0x06
#define BQ27220_REG_VOLT        0x08
#define BQ27220_REG_BAT_STATUS  0x0A
#define BQ27220_REG_CURR        0x0C
#define BQ27220_REG_REMAIN_CAP  0x10
#define BQ27220_REG_FULL_CAP    0x12
#define BQ27220_REG_CHARGE_PCT  0x2C
#define BQ27220_REG_HEALTH_PCT  0x2E
#define BQ27220_REG_DESIGN_CAP  0x3C

/* I2C configuration — shared with NFC (PN532) and Qwiic */
#if defined(BOARD_PIN_QWIIC_SDA) && defined(BOARD_PIN_QWIIC_SCL)
#define BQ_I2C_FREQ_HZ  100000
#define BQ_I2C_TIMEOUT  (1000 / portTICK_PERIOD_MS)
#else
#define BQ_I2C_PORT     I2C_NUM_0
#define BQ_I2C_SDA      8
#define BQ_I2C_SCL      18
#define BQ_I2C_FREQ_HZ  100000
#define BQ_I2C_TIMEOUT  (1000 / portTICK_PERIOD_MS)
#endif

static bool bq27220_initialized = false;
static bool bq27220_present = false;

static bool bq27220_i2c_read(uint8_t reg, uint8_t* data, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ27220_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ27220_ADDR << 1) | I2C_MASTER_READ, true);
    if(len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BQ_I2C_PORT, cmd, BQ_I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static uint16_t bq27220_read_word(uint8_t reg) {
    uint8_t data[2] = {0};
    if(!bq27220_i2c_read(reg, data, 2)) {
        return 0;
    }
    return ((uint16_t)data[1] << 8) | data[0];
}

bool furi_hal_bq27220_init(void) {
    if(bq27220_initialized) return bq27220_present;

    bq27220_initialized = true;

    /* I2C bus assumed already initialized by furi_hal_power_init() */

    /* Probe: check if device ACKs at 0x55 */
    i2c_cmd_handle_t probe_cmd = i2c_cmd_link_create();
    i2c_master_start(probe_cmd);
    i2c_master_write_byte(probe_cmd, (BQ27220_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(probe_cmd);
    esp_err_t probe_err = i2c_master_cmd_begin(BQ_I2C_PORT, probe_cmd, BQ_I2C_TIMEOUT);
    i2c_cmd_link_delete(probe_cmd);

    if(probe_err != ESP_OK) {
        ESP_LOGW(TAG, "BQ27220 not found at 0x%02X (no ACK)", BQ27220_ADDR);
        bq27220_present = false;
        return false;
    }

    /* Device present — read key registers */
    uint16_t voltage = bq27220_read_word(BQ27220_REG_VOLT);
    uint16_t pct = bq27220_read_word(BQ27220_REG_CHARGE_PCT);
    uint16_t design_cap = bq27220_read_word(BQ27220_REG_DESIGN_CAP);

    bq27220_present = true;
    ESP_LOGI(TAG, "BQ27220 found: voltage=%umV charge=%u%% design_cap=%umAh",
        voltage, pct, design_cap);

    return bq27220_present;
}

bool furi_hal_bq27220_is_present(void) {
    return bq27220_present;
}

uint8_t furi_hal_bq27220_get_charge_pct(void) {
    if(!bq27220_present) return 0;
    uint16_t pct = bq27220_read_word(BQ27220_REG_CHARGE_PCT);
    if(pct > 100) pct = 100;
    return (uint8_t)pct;
}

bool furi_hal_bq27220_is_charging(void) {
    if(!bq27220_present) return false;
    uint16_t status = bq27220_read_word(BQ27220_REG_BAT_STATUS);
    /* DSG bit (bit 0): 1 = discharging, 0 = charging */
    return (status & 0x0001) == 0;
}

uint16_t furi_hal_bq27220_get_voltage_mv(void) {
    if(!bq27220_present) return 0;
    return bq27220_read_word(BQ27220_REG_VOLT);
}

int16_t furi_hal_bq27220_get_current_ma(void) {
    if(!bq27220_present) return 0;
    return (int16_t)bq27220_read_word(BQ27220_REG_CURR);
}

uint8_t furi_hal_bq27220_get_health_pct(void) {
    if(!bq27220_present) return 0;
    uint16_t health = bq27220_read_word(BQ27220_REG_HEALTH_PCT);
    if(health > 100) health = 100;
    return (uint8_t)health;
}

uint16_t furi_hal_bq27220_get_temperature_raw(void) {
    if(!bq27220_present) return 0;
    return bq27220_read_word(BQ27220_REG_TEMP);
}

uint16_t furi_hal_bq27220_get_remaining_capacity_mah(void) {
    if(!bq27220_present) return 0;
    return bq27220_read_word(BQ27220_REG_REMAIN_CAP);
}

uint16_t furi_hal_bq27220_get_full_charge_capacity_mah(void) {
    if(!bq27220_present) return 0;
    return bq27220_read_word(BQ27220_REG_FULL_CAP);
}

uint16_t furi_hal_bq27220_get_design_capacity_mah(void) {
    if(!bq27220_present) return 0;
    return bq27220_read_word(BQ27220_REG_DESIGN_CAP);
}

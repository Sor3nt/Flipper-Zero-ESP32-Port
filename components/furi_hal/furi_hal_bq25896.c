/**
 * @file furi_hal_bq25896.c
 * BQ25896 charger driver — ESP-IDF I2C (legacy driver API)
 * Ported from STM32 Flipper firmware (lib/drivers/bq25896.c).
 *
 * I2C address: 0x6B
 * Bus: Qwiic/Grove (shared with BQ27220)
 */

#include "furi_hal_bq25896.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "BQ25896";

#define BQ25896_ADDR    0x6B
#define BQ_I2C_PORT     I2C_NUM_0
#define BQ_I2C_TIMEOUT  (1000 / portTICK_PERIOD_MS)

/* Key registers */
#define REG00   0x00    /* Input current limit, EN_HIZ */
#define REG02   0x02    /* ADC control: CONV_START, CONV_RATE */
#define REG03   0x03    /* SYS config: OTG, CHG enable */
#define REG04   0x04    /* Fast charge current limit */
#define REG05   0x05    /* Pre-charge / termination current */
#define REG06   0x06    /* Charge voltage limit (VREG) */
#define REG07   0x07    /* Charge timer, watchdog */
#define REG0A   0x0A    /* Boost voltage/current */
#define REG0B   0x0B    /* Status: VBUS, CHG, PG */
#define REG0C   0x0C    /* Fault flags */
#define REG0E   0x0E    /* ADC: battery voltage */
#define REG0F   0x0F    /* ADC: system voltage */
#define REG10   0x10    /* ADC: NTC percentage */
#define REG11   0x11    /* ADC: VBUS voltage */
#define REG12   0x12    /* ADC: charge current */
#define REG14   0x14    /* Reset, device ID */

static bool bq25896_present = false;

static bool bq25896_read_reg(uint8_t reg, uint8_t* val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25896_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25896_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BQ_I2C_PORT, cmd, BQ_I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool bq25896_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BQ25896_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(BQ_I2C_PORT, cmd, BQ_I2C_TIMEOUT);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

bool furi_hal_bq25896_init(void) {
    /* I2C bus assumed already initialized by BQ27220 */

    /* Read device ID to check presence */
    uint8_t reg14 = 0;
    if(!bq25896_read_reg(REG14, &reg14)) {
        ESP_LOGW(TAG, "BQ25896 not found at 0x%02X", BQ25896_ADDR);
        return false;
    }

    uint8_t dev_id = (reg14 >> 3) & 0x07;
    ESP_LOGI(TAG, "BQ25896 found: REG14=0x%02X dev_id=%u", reg14, dev_id);

    /* Skip hard reset — preserves charger state across reboots.
     * STM32 firmware does reset but has dedicated I2C bus; shared bus is more sensitive. */

    /* Enable ADC continuous conversion (REG02: CONV_START=1, CONV_RATE=1) */
    uint8_t reg02 = 0;
    bq25896_read_reg(REG02, &reg02);
    reg02 |= 0xC0; /* bits 7:6 = CONV_START | CONV_RATE */
    bq25896_write_reg(REG02, reg02);

    /* Disable watchdog timer (REG07: WATCHDOG bits 5:4 = 00) */
    uint8_t reg07 = 0;
    bq25896_read_reg(REG07, &reg07);
    reg07 &= ~0x30;
    bq25896_write_reg(REG07, reg07);

    /* Set charge voltage limit to 4.208V (default safe for most Li-ion) */
    furi_hal_bq25896_set_vreg_voltage_mv(4208);

    /* Set charge current limit (REG04): 1200mA — safe for T-Embed battery
     * Default is 2048mA which is too high for small batteries, causing
     * premature charge termination (current drops below ITERM immediately) */
    uint8_t reg04 = bq25896_read_reg(REG04, &reg04);
    reg04 &= 0x80; /* preserve EN_PUMPX (bit 7) */
    reg04 |= (BQ25896_CHARGE_LIMIT >> 6); /* divide by 64, steps of 64mA */
    bq25896_write_reg(REG04, reg04);

    /* Set pre-charge and termination current (REG05):
     * Pre-charge = 128mA (default), Termination = 64mA (lowest) */
    uint8_t reg05 = 0;
    bq25896_read_reg(REG05, &reg05);
    reg05 = 0x10; /* IPRECHG=128mA (bits 7:4=0001), ITERM=64mA (bits 3:0=0000) */
    bq25896_write_reg(REG05, reg05);

    /* Enable charging (may be disabled after watchdog timeout during reboot) */
    bq25896_present = true;
    furi_hal_bq25896_enable_charging();

    /* Log full config */
    bq25896_read_reg(REG04, &reg04);
    bq25896_read_reg(REG05, &reg05);
    ESP_LOGI(TAG, "Charging enabled: VREG=%umV ICHG=%umA ITERM=%umA",
             furi_hal_bq25896_get_vreg_voltage_mv(),
             (reg04 & 0x7F) * 64, /* ICHG formula */
             (reg05 & 0x0F) * 64 + 64 /* ITERM formula */);
    return true;
}

bool furi_hal_bq25896_is_present(void) {
    return bq25896_present;
}

bool furi_hal_bq25896_is_charging(void) {
    if(!bq25896_present) return false;
    uint8_t reg0b = 0;
    bq25896_read_reg(REG0B, &reg0b);
    uint8_t chg_stat = (reg0b >> 3) & 0x03;
    /* 00=Not charging, 01=Pre-charge, 10=Fast charge, 11=Charge done */
    return chg_stat == 0x01 || chg_stat == 0x02;
}

bool furi_hal_bq25896_is_charging_done(void) {
    if(!bq25896_present) return false;
    uint8_t reg0b = 0;
    bq25896_read_reg(REG0B, &reg0b);
    return ((reg0b >> 3) & 0x03) == 0x03;
}

void furi_hal_bq25896_enable_charging(void) {
    if(!bq25896_present) return;
    uint8_t reg03 = 0;
    bq25896_read_reg(REG03, &reg03);
    reg03 |= 0x10; /* CHG_CONFIG bit 4 = 1 */
    bq25896_write_reg(REG03, reg03);
}

void furi_hal_bq25896_disable_charging(void) {
    if(!bq25896_present) return;
    uint8_t reg03 = 0;
    bq25896_read_reg(REG03, &reg03);
    reg03 &= ~0x10; /* CHG_CONFIG bit 4 = 0 */
    bq25896_write_reg(REG03, reg03);
}

uint16_t furi_hal_bq25896_get_vbus_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg11 = 0;
    bq25896_read_reg(REG11, &reg11);
    /* VBUS = 2600mV + REG11[6:0] × 100mV */
    return 2600 + (reg11 & 0x7F) * 100;
}

uint16_t furi_hal_bq25896_get_vbat_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg0e = 0;
    bq25896_read_reg(REG0E, &reg0e);
    /* VBAT = 2304mV + REG0E[6:0] × 20mV */
    return 2304 + (reg0e & 0x7F) * 20;
}

uint16_t furi_hal_bq25896_get_vbat_current_ma(void) {
    if(!bq25896_present) return 0;
    uint8_t reg12 = 0;
    bq25896_read_reg(REG12, &reg12);
    /* ICHG = REG12[6:0] × 50mA */
    return (reg12 & 0x7F) * 50;
}

int32_t furi_hal_bq25896_get_temperature_mc(void) {
    if(!bq25896_present) return 25000; /* default 25°C */
    uint8_t reg10 = 0;
    bq25896_read_reg(REG10, &reg10);
    /* NTC percentage: TSPCT = 21% + REG10[6:0] × 0.465% */
    /* Temperature approximation from STM32 firmware: (71 - NTCPCT) / 0.6 */
    float ntc_pct = 21.0f + (float)(reg10 & 0x7F) * 0.465f;
    float temp_c = (71.0f - ntc_pct) / 0.6f;
    return (int32_t)(temp_c * 1000.0f);
}

uint16_t furi_hal_bq25896_get_vreg_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg06 = 0;
    bq25896_read_reg(REG06, &reg06);
    /* VREG = 3840mV + REG06[7:2] × 16mV */
    return 3840 + ((reg06 >> 2) & 0x3F) * 16;
}

void furi_hal_bq25896_set_vreg_voltage_mv(uint16_t vreg_mv) {
    if(!bq25896_present) return;
    if(vreg_mv < 3840) vreg_mv = 3840;
    if(vreg_mv > 4608) vreg_mv = 4608;
    uint8_t reg06 = 0;
    bq25896_read_reg(REG06, &reg06);
    uint8_t vreg_code = (vreg_mv - 3840) / 16;
    reg06 = (reg06 & 0x03) | (vreg_code << 2);
    bq25896_write_reg(REG06, reg06);
}

void furi_hal_bq25896_enable_otg(void) {
    if(!bq25896_present) return;
    uint8_t reg03 = 0;
    bq25896_read_reg(REG03, &reg03);
    reg03 |= 0x20; /* OTG_CONFIG bit 5 = 1 */
    reg03 &= ~0x10; /* Disable charging while OTG */
    bq25896_write_reg(REG03, reg03);
}

void furi_hal_bq25896_disable_otg(void) {
    if(!bq25896_present) return;
    uint8_t reg03 = 0;
    bq25896_read_reg(REG03, &reg03);
    reg03 &= ~0x20; /* OTG_CONFIG bit 5 = 0 */
    bq25896_write_reg(REG03, reg03);
}

bool furi_hal_bq25896_is_otg_enabled(void) {
    if(!bq25896_present) return false;
    uint8_t reg03 = 0;
    bq25896_read_reg(REG03, &reg03);
    return (reg03 & 0x20) != 0;
}

#include "bmi270.h"
#include "bmi270_config.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

#define TAG "BMI270"
#define BMI270_ADDR        0x68
#define BMI270_I2C_PORT    KB_I2C_PORT /* I2C_NUM_0 on this board */

/* Registers */
#define REG_CHIP_ID         0x00
#define REG_ACC_DATA        0x0C /* X_LSB..Z_MSB, 6 bytes */
#define REG_INTERNAL_STATUS 0x21
#define REG_ACC_CONF        0x40
#define REG_ACC_RANGE       0x41
#define REG_INIT_CTRL       0x59
#define REG_INIT_ADDR_0     0x5B
#define REG_INIT_ADDR_1     0x5C
#define REG_INIT_DATA       0x5E
#define REG_PWR_CONF        0x7C
#define REG_PWR_CTRL        0x7D
#define REG_CMD             0x7E

#define BMI270_CHIP_ID      0x24
#define CMD_SOFTRESET       0xB6

static bool s_present = false;

static esp_err_t wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_write_to_device(BMI270_I2C_PORT, BMI270_ADDR, b, 2, pdMS_TO_TICKS(50));
}
static esp_err_t rd(uint8_t reg, uint8_t* buf, size_t n) {
    return i2c_master_write_read_device(
        BMI270_I2C_PORT, BMI270_ADDR, &reg, 1, buf, n, pdMS_TO_TICKS(50));
}

bool bmi270_is_present(void) {
    return s_present;
}

bool bmi270_init(void) {
    s_present = false;
    uint8_t id = 0;
    if(rd(REG_CHIP_ID, &id, 1) != ESP_OK || id != BMI270_CHIP_ID) {
        ESP_LOGW(TAG, "no BMI270 (CHIP_ID=0x%02X, want 0x24)", id);
        return false;
    }

    wr(REG_CMD, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* disable advanced power save, then upload config (BMI270 needs the blob) */
    wr(REG_PWR_CONF, 0x00);
    esp_rom_delay_us(450);
    wr(REG_INIT_CTRL, 0x00);
    wr(REG_INIT_ADDR_0, 0x00);
    wr(REG_INIT_ADDR_1, 0x00);

    /* burst-write the 8 KB blob to INIT_DATA in chunks, advancing the internal
     * word pointer (addr in words = off/2) before each chunk */
    const size_t CH = 32;
    uint8_t buf[1 + 32];
    buf[0] = REG_INIT_DATA;
    for(size_t off = 0; off < BMI270_CONFIG_SIZE; off += CH) {
        size_t n = (BMI270_CONFIG_SIZE - off < CH) ? (BMI270_CONFIG_SIZE - off) : CH;
        wr(REG_INIT_ADDR_0, (uint8_t)((off / 2) & 0x0F));
        wr(REG_INIT_ADDR_1, (uint8_t)((off / 2) >> 4));
        for(size_t i = 0; i < n; i++) buf[1 + i] = bmi270_config_file[off + i];
        if(i2c_master_write_to_device(
               BMI270_I2C_PORT, BMI270_ADDR, buf, 1 + n, pdMS_TO_TICKS(50)) != ESP_OK) {
            ESP_LOGE(TAG, "config upload failed at %u", (unsigned)off);
            return false;
        }
    }
    wr(REG_INIT_CTRL, 0x01);

    /* wait for init_ok */
    bool ok = false;
    for(int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t st = 0;
        if(rd(REG_INTERNAL_STATUS, &st, 1) == ESP_OK && (st & 0x0F) == 0x01) {
            ok = true;
            break;
        }
    }
    if(!ok) {
        ESP_LOGE(TAG, "init_ok never set");
        return false;
    }

    /* enable accel: 100Hz normal filter, +/-2g */
    wr(REG_PWR_CTRL, 0x04);
    wr(REG_ACC_CONF, 0xA8);
    wr(REG_ACC_RANGE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));

    s_present = true;
    ESP_LOGI(TAG, "BMI270 ready (accel 100Hz +/-2g)");
    return true;
}

bool bmi270_read_accel(int16_t* x, int16_t* y, int16_t* z) {
    if(!s_present) return false;
    uint8_t d[6];
    if(rd(REG_ACC_DATA, d, 6) != ESP_OK) return false;
    *x = (int16_t)((d[1] << 8) | d[0]);
    *y = (int16_t)((d[3] << 8) | d[2]);
    *z = (int16_t)((d[5] << 8) | d[4]);
    return true;
}

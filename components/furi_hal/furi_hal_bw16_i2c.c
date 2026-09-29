#include "furi_hal_bw16_i2c.h"
#include "sdkconfig.h"
#include "boards/board.h"
#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>

bool furi_hal_bw16_i2c_available(void) {
#if CONFIG_T_EMBED_BW16_I2C && defined(BOARD_EXTERNAL_UART_TX) && BOARD_PIN_NFC_SDA == 8 && BOARD_PIN_NFC_SCL == 18
    return true;
#else
    return false;
#endif
}

/* Borrow the bus initialized by power management. The IDF legacy driver
 * serializes each transaction with its bus mutex, including NFC/BQ requests.
 * Never install, reconfigure or reset this shared bus from a FAP. */
esp_err_t furi_hal_bw16_i2c_write(const uint8_t* data, size_t size) {
    if(!furi_hal_bw16_i2c_available()) return ESP_ERR_NOT_SUPPORTED;
    if(!data || size != FURI_HAL_BW16_I2C_FRAME_SIZE) return ESP_ERR_INVALID_ARG;
    return i2c_master_write_to_device(I2C_NUM_0, FURI_HAL_BW16_I2C_ADDRESS,
        data, size, pdMS_TO_TICKS(30) ? pdMS_TO_TICKS(30) : 1);
}

esp_err_t furi_hal_bw16_i2c_read(uint8_t* data, size_t size) {
    if(!furi_hal_bw16_i2c_available()) return ESP_ERR_NOT_SUPPORTED;
    if(!data || size != FURI_HAL_BW16_I2C_FRAME_SIZE) return ESP_ERR_INVALID_ARG;
    return i2c_master_read_from_device(I2C_NUM_0, FURI_HAL_BW16_I2C_ADDRESS,
        data, size, pdMS_TO_TICKS(30) ? pdMS_TO_TICKS(30) : 1);
}

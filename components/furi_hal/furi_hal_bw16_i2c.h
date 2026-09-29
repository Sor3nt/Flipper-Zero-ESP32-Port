#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FURI_HAL_BW16_I2C_ADDRESS 0x42
#define FURI_HAL_BW16_I2C_FRAME_SIZE 64
bool furi_hal_bw16_i2c_available(void);
esp_err_t furi_hal_bw16_i2c_write(const uint8_t* data, size_t size);
esp_err_t furi_hal_bw16_i2c_read(uint8_t* data, size_t size);
#ifdef __cplusplus
}
#endif

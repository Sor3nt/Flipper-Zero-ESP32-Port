#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Exported by the isolated T-Embed Plus I2C firmware profile.
 * These calls borrow its existing 100 kHz bus with a 30 ms timeout.
 * A write and a read are separate bus transactions. */
bool furi_hal_bw16_i2c_available(void);
int furi_hal_bw16_i2c_write(const uint8_t* data, size_t size);
int furi_hal_bw16_i2c_read(uint8_t* data, size_t size);

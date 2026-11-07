#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Minimal accel-only BMI270 driver on the M5Stick S3 internal I2C bus (I2C_NUM_0). */

bool bmi270_init(void);                 /* full bring-up; false if absent/failed */
bool bmi270_is_present(void);           /* true after a successful init */
bool bmi270_read_accel(int16_t* x, int16_t* y, int16_t* z); /* raw signed 16-bit */

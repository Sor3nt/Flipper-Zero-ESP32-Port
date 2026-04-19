#pragma once

#include <stdint.h>

#define FURI_HAL_RANDOM_MAX 0xFFFFFFFFU

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_random_init(void);
uint32_t furi_hal_random_get(void);
void furi_hal_random_fill_buf(uint8_t* buf, uint32_t len);

#ifdef __cplusplus
}
#endif

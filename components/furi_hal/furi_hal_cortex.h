#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t start;
    uint32_t value;
} FuriHalCortexTimer;

void furi_hal_cortex_init_early(void);
void furi_hal_cortex_delay_us(uint32_t microseconds);
uint32_t furi_hal_cortex_instructions_per_microsecond(void);
FuriHalCortexTimer furi_hal_cortex_timer_get(uint32_t timeout_us);
bool furi_hal_cortex_timer_is_expired(FuriHalCortexTimer cortex_timer);
void furi_hal_cortex_timer_wait(FuriHalCortexTimer cortex_timer);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t start;
    uint32_t end;
    int8_t power_limit;
    uint8_t duty_cycle;
} FuriHalRegionBand;

typedef struct {
    char country_code[4];
    uint16_t bands_count;
    FuriHalRegionBand bands[];
} FuriHalRegion;

const FuriHalRegion* furi_hal_region_get(void);
void furi_hal_region_set(FuriHalRegion* region);
bool furi_hal_region_is_provisioned(void);
const char* furi_hal_region_get_name(void);
bool furi_hal_region_is_frequency_allowed(uint32_t frequency);
const FuriHalRegionBand* furi_hal_region_get_band(uint32_t frequency);

#ifdef __cplusplus
}
#endif

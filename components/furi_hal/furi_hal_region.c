#include "furi_hal_region.h"

#include <stddef.h>

#include <core/common_defines.h>

static const FuriHalRegion furi_hal_region_zero = {
    .country_code = "00",
    .bands_count = 1,
    .bands =
        {
            {
                .start = 0,
                .end = 1000000000,
                .power_limit = 12,
                .duty_cycle = 50,
            },
        },
};

const FuriHalRegion* furi_hal_region_get(void) {
    return &furi_hal_region_zero;
}

void furi_hal_region_set(FuriHalRegion* region) {
    UNUSED(region);
}

bool furi_hal_region_is_provisioned(void) {
    return true;
}

const char* furi_hal_region_get_name(void) {
    return "00";
}

bool furi_hal_region_is_frequency_allowed(uint32_t frequency) {
    UNUSED(frequency);
    return true;
}

const FuriHalRegionBand* furi_hal_region_get_band(uint32_t frequency) {
    if((frequency >= furi_hal_region_zero.bands[0].start) &&
       (frequency <= furi_hal_region_zero.bands[0].end)) {
        return &furi_hal_region_zero.bands[0];
    }

    return NULL;
}

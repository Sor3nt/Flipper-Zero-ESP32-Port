#include "furi_hal_psram.h"

#include <furi.h>
#include <sdkconfig.h>
#include <string.h>

#include <esp_heap_caps.h>
#if CONFIG_SPIRAM
#include <esp_psram.h>
#endif

bool furi_hal_psram_is_available(void) {
#if CONFIG_SPIRAM
    /* CONFIG_SPIRAM only says the build supports it: with SPIRAM_IGNORE_NOTFOUND the
     * firmware also boots on a module without the chip, so ask the driver. */
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

#if CONFIG_SPIRAM
static const char* furi_hal_psram_get_type(void) {
#if CONFIG_SPIRAM_MODE_OCT
    return "Octal SPI (OPI)";
#elif CONFIG_SPIRAM_MODE_QUAD
    return "Quad SPI (QSPI)";
#elif CONFIG_SPIRAM_MODE_HEX
    return "Hex SPI (HPI)";
#else
    return "SPI";
#endif
}
#endif

bool furi_hal_psram_get_info(FuriHalPsramInfo* info) {
    furi_check(info);
    memset(info, 0, sizeof(*info));

    if(!furi_hal_psram_is_available()) return false;

#if CONFIG_SPIRAM
    info->type = furi_hal_psram_get_type();
#ifdef CONFIG_SPIRAM_SPEED
    info->speed_mhz = CONFIG_SPIRAM_SPEED;
#endif
    /* Chip size: part of it holds the code/rodata/bss moved to PSRAM, so it is
     * larger than the heap; free is what the heap can still hand out. */
    info->total = esp_psram_get_size();
    info->free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if(info->free > info->total) info->free = info->total;
#endif

    return true;
}

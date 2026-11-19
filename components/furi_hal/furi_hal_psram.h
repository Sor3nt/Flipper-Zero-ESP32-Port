/**
 * @file furi_hal_psram.h
 * External PSRAM (SPI RAM) presence and usage.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Bus type, e.g. "Octal SPI (OPI)" (never NULL when the info is valid). */
    const char* type;
    /** Bus clock in MHz, 0 when unknown. */
    uint32_t speed_mhz;
    /** Usable size of the PSRAM chip in bytes. */
    size_t total;
    /** Bytes still free in the PSRAM heap. */
    size_t free;
} FuriHalPsramInfo;

/** True when this build supports PSRAM and the chip was found and set up at boot.
 *  False on boards without PSRAM (ESP32-C6 boards, Cardputer-ADV) and on boards
 *  whose PSRAM chip is missing (CONFIG_SPIRAM_IGNORE_NOTFOUND). */
bool furi_hal_psram_is_available(void);

/** Fill info with the PSRAM type and usage.
 *  @return false (and a zeroed info) when PSRAM is not available. */
bool furi_hal_psram_get_info(FuriHalPsramInfo* info);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>

#include <property.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_info_get_api_version(uint16_t* major, uint16_t* minor);

/** Einmalig beim Boot (furi_hal_init, Main-Task mit internem Stack): cached die
 *  OTA-Infos (laufende Partition, OTA-Slot vorhanden). furi_hal_info_get() laeuft
 *  spaeter auf dem 4-6 KB RPC-Worker-Stack und darf dort keine
 *  esp_partition/esp_ota-Aufrufe mehr machen (Stack-Overflow → Reset). */
void furi_hal_info_init(void);

void furi_hal_info_get(PropertyValueCallback callback, char sep, void* context);

#ifdef __cplusplus
}
#endif

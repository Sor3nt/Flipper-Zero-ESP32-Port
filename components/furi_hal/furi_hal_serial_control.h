/**
 * @file furi_hal_serial_control.h
 * @brief Serial handle acquisition for the ESP32 port.
 *
 * API-compatible subset of upstream Flipper's furi_hal_serial_control. A handle must be
 * acquired before use and released afterwards; acquisition is exclusive per bus.
 */
#pragma once

#include "furi_hal_serial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Expansion-detection callback type (API compatibility; expansion is handled elsewhere). */
typedef void (*FuriHalSerialControlExpansionCallback)(void* context);

/**
 * @brief Acquire exclusive access to a serial bus.
 * @param serial_id  Which UART (only FuriHalSerialIdUsart is wired on this port).
 * @return handle, or NULL if the bus is busy or unavailable.
 */
FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId serial_id);

/** Release a handle acquired with furi_hal_serial_control_acquire(). */
void furi_hal_serial_control_release(FuriHalSerialHandle* handle);

/** Register an expansion-detection callback (no-op on this port). */
void furi_hal_serial_control_set_expansion_callback(
    FuriHalSerialId serial_id,
    FuriHalSerialControlExpansionCallback callback,
    void* context);

#ifdef __cplusplus
}
#endif

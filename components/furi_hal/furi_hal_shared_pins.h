#pragma once
#include <stdbool.h>

/* Cooperative lease for the NRF/RFID/BW16 pins. This cannot isolate hardware.
 * Owner must remain alive until release. Legacy third-party FAPs can bypass it. */
bool furi_hal_shared_pins_acquire(const void* owner);
void furi_hal_shared_pins_release(const void* owner);
/* Save/restore the T-Embed GPIO43/44 mux, pulls, output and UART1 RX route.
 * Call only with the lease held, UART1 unowned, and BW16 physically unplugged. */
bool furi_hal_shared_pins_save(const void* owner);
void furi_hal_shared_pins_restore(const void* owner);
bool furi_hal_shared_pins_is_saved(const void* owner);

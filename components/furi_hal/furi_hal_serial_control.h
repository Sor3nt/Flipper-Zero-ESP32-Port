#pragma once

#include "furi_hal_serial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_serial_control_init(void);
void furi_hal_serial_control_deinit(void);

FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId serial_id);

void furi_hal_serial_control_release(FuriHalSerialHandle* handle);

bool furi_hal_serial_control_is_busy(FuriHalSerialId serial_id);

#ifdef __cplusplus
}
#endif

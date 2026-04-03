#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_debug_enable(void);
void furi_hal_debug_disable(void);
bool furi_hal_debug_is_gdb_session_active(void);

#ifdef __cplusplus
}
#endif

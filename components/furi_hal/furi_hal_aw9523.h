#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void furi_hal_aw9523_init(void);
bool furi_hal_aw9523_set_idle(void);
bool furi_hal_aw9523_set_rx(void);
bool furi_hal_aw9523_set_tx(void);
bool furi_hal_aw9523_is_present(void);

#ifdef __cplusplus
}
#endif

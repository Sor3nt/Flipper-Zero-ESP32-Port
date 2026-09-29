#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* True only in firmware built for the dedicated external UART header. */
bool furi_hal_external_uart_available(void);
#ifdef __cplusplus
}
#endif

#pragma once
#include <sdkconfig.h>

/* I2C is shared by both hardware variants; NRF ownership of GPIO43/44 is
 * identified by the external UART being disabled, not by I2C alone. */
#if CONFIG_T_EMBED_BW16_I2C && !CONFIG_T_EMBED_EXTERNAL_UART
#define WDG_MAP_PLUS_PROFILE 1
#else
#define WDG_MAP_PLUS_PROFILE 0
#endif
#if CONFIG_T_EMBED_BW16_I2C && CONFIG_T_EMBED_EXTERNAL_UART
#define WDG_MAP_UART_I2C_PROFILE 1
#else
#define WDG_MAP_UART_I2C_PROFILE 0
#endif

#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t gps_mode; /* 0 OFF, 1 AUTO (listen only), 2 ON (listen only) */
    uint32_t gps_uart, gps_baud;
    int32_t gps_rx, gps_tx;
    bool isolated_uart_pins; /* Selected pins electrically isolated from board peripherals. */
} WardriverConfig;
void wardriver_config_defaults(WardriverConfig* config);
bool wardriver_config_load(WardriverConfig* config);
bool wardriver_config_save(const WardriverConfig* config);

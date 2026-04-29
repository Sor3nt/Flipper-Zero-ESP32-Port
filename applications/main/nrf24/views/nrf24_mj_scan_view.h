#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t current_channel;
    uint8_t target_count; /* 0..16 */
    uint16_t sweep_count;
    bool running;
    bool hardware_ok;
    bool no_targets_msg; /* show "No targets" overlay just before leaving */
    char last_target_label[24]; /* "AB:CD:EF (LG) ch72" */
} Nrf24MjScanModel;

typedef enum {
    Nrf24MjScanEventStop = 1,
} Nrf24MjScanEvent;

View* nrf24_mj_scan_view_alloc(void);
void nrf24_mj_scan_view_free(View* view);

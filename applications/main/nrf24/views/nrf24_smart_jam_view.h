#pragma once

#include <gui/view.h>

#define NRF24_SMART_JAM_TARGETS 8

typedef enum {
    SmartJamPhaseScan,
    SmartJamPhaseJam,
} SmartJamPhase;

typedef struct {
    SmartJamPhase phase;

    /* Scan-Phase */
    uint8_t scan_progress;   /* 0..100 % */
    uint16_t sweep_count;

    /* Jam-Phase */
    uint8_t targets[NRF24_SMART_JAM_TARGETS];
    uint16_t target_hits[NRF24_SMART_JAM_TARGETS];
    uint8_t target_count;
    uint8_t current_target_idx;
    uint8_t current_channel;
    uint32_t hop_count;
    bool running;
    bool wide_mode;

    bool hardware_ok;
} Nrf24SmartJamModel;

typedef enum {
    Nrf24SmartJamEventToggle = 1,
    Nrf24SmartJamEventToggleMode,
} Nrf24SmartJamEvent;

View* nrf24_smart_jam_view_alloc(void);
void nrf24_smart_jam_view_free(View* view);

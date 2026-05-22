#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char preset_name[16]; /* short label, e.g. "BLE Adv" */
    uint8_t channel; /* current NRF24 channel */
    uint32_t hop_count;
    uint16_t dwell_us; /* per-channel dwell time */
    bool flooding; /* false = constant carrier, true = data flooding */
    bool low_rate; /* flood only: true = 250 kbps, false = 2 Mbps */
    bool running;
    bool hardware_ok;
} Nrf24PresetJamModel;

typedef enum {
    Nrf24PresetJamEventToggle = 1, /* OK: start / stop */
    Nrf24PresetJamEventToggleStrategy, /* Left: CW <-> Flood */
    Nrf24PresetJamEventToggleRate, /* Right: 2 Mbps <-> 250 kbps (flood) */
    Nrf24PresetJamEventDwellUp, /* Up: dwell + */
    Nrf24PresetJamEventDwellDown, /* Down: dwell - */
} Nrf24PresetJamEvent;

View* nrf24_preset_jam_view_alloc(void);
void nrf24_preset_jam_view_free(View* view);

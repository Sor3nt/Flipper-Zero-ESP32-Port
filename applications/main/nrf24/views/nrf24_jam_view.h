#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char source_label[12]; /* "Protocol" / "Manual" / "WiFi" / "Activity" */
    char selection_label[20]; /* "BLE" / "CH 50" / "MyAP" / "Smart 8ch" */
    uint8_t channel; /* current NRF24 channel */
    uint32_t hop_count;
    uint32_t sweeps;
    uint32_t elapsed_ms;
    uint32_t ch_per_sec;
    uint16_t dwell_us;
    uint8_t strategy; /* 0 = CW, 1 = FLOOD, 2 = TURBO */
    uint8_t pa_level; /* 0..3 MIN/LOW/HIGH/MAX */
    bool random_hop;
    bool running;
    bool hardware_ok;
    bool can_jam; /* false → no channels (e.g. WiFi with no AP / needs scan) */
    bool show_scan; /* WiFi/Activity not yet scanned → center button = "Scan" */
    /* Active channel set for the band visualisation (filled by the worker). */
    uint8_t channels[128];
    uint8_t band_count;
} Nrf24JamModel;

typedef enum {
    Nrf24JamEventToggle = 1, /* OK: start / stop (or "Scan" when source unready) */
    Nrf24JamEventSelectNext, /* Right: next selection within source */
    Nrf24JamEventCycleSource, /* Down: next source type */
    Nrf24JamEventConfig, /* Up / Left: open config editor */
    Nrf24JamEventRescan, /* OK-long: re-scan WiFi/Activity (else open config) */
} Nrf24JamEvent;

View* nrf24_jam_view_alloc(void);
void nrf24_jam_view_free(View* view);

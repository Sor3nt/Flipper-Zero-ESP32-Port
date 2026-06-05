#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unified jammer "channel source": every jam method reduces to producing a set
 * of NRF24 channels and then jamming it with the shared engine. The four
 * sources differ only in how that channel set is derived. */
typedef enum {
    Nrf24SourceProtocol = 0, /* curated protocol preset (BLE/BT/WiFi-band/…) */
    Nrf24SourceWifi, /* range derived from a scanned WiFi AP */
    Nrf24SourceActivity, /* top-N channels from an RPD activity scan */
    Nrf24SourceCount,
} Nrf24JamSource;

#define NRF24_ACTIVITY_TARGETS 8
#define NRF24_ACTIVITY_CH_MIN  0
#define NRF24_ACTIVITY_CH_MAX  79

/* Live selection + scan-cache state for the unified jammer. Lives in Nrf24App
 * so it survives the scan / config sub-scenes (which tear the jam scene down
 * and back up via the scene manager). */
typedef struct {
    uint8_t source; /* Nrf24JamSource */
    uint8_t protocol; /* Nrf24JamPreset — Protocol selection */
    uint8_t wifi_index; /* index into Nrf24App.wifi_aps — WiFi selection */
    bool wifi_scanned; /* WiFi scan cached this session */
    bool activity_wide; /* Activity: false = Top-N, true = Wide sweep */
    bool activity_scanned; /* Activity scan cached this session */
    uint8_t activity_count; /* number of Top-N channels (0 = none → Wide) */
    uint8_t activity_ch[NRF24_ACTIVITY_TARGETS];
    uint16_t activity_hits[NRF24_ACTIVITY_TARGETS];
    bool resume_running; /* restore run-state after a scan/config sub-scene */
} Nrf24JamState;

/* Forward decl — the source helpers operate on the whole app (channel set,
 * WiFi AP list, jam state). */
typedef struct Nrf24App Nrf24App;

void nrf24_jam_state_init(Nrf24JamState* st);

/* "Protocol" / "Manual" / "WiFi" / "Activity". */
const char* nrf24_source_type_label(uint8_t source);

/* Human selection label for the current source, e.g. "BLE", "CH 50",
 * "MyAP", "Smart 8ch" / "Wide". */
void nrf24_source_selection_label(Nrf24App* app, char* buf, size_t cap);

/* Fill out[] with the channel set for the current source + selection.
 * Returns the channel count (0 if nothing to jam, e.g. WiFi with no AP). */
size_t nrf24_source_fill_channels(Nrf24App* app, uint8_t* out, size_t cap);

/* Left/Right: change the selection within the current source (dir = +1/-1). */
void nrf24_source_select(Nrf24App* app, int dir);

/* Down: advance to the next source type (Protocol→Manual→WiFi→Activity→…). */
void nrf24_source_cycle_type(Nrf24App* app);

/* True if the current source still needs a scan before it can produce
 * channels (WiFi without APs, Activity without a scan). */
bool nrf24_source_needs_scan(Nrf24App* app);

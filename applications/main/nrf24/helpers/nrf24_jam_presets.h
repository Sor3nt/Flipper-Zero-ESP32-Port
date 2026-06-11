#pragma once

#include <stddef.h>
#include <stdint.h>

/* Protocol-targeted jammer presets, ported from Bruce's nrf_jammer.cpp.
 * Each preset maps to a curated NRF24 channel list (2400 + ch MHz) tuned to
 * the band a given protocol lives in. Full sweeps every channel, Drone hops
 * randomly (FHSS target). */
typedef enum {
    Nrf24JamPresetFull, /* sequential sweep 0..124 */
    Nrf24JamPresetWifi, /* WiFi ch 1/6/11 sub-channels */
    Nrf24JamPresetBle, /* BLE data channels */
    Nrf24JamPresetBleAdv, /* BLE advertising ch 37/38/39 */
    Nrf24JamPresetBluetooth, /* BT classic FHSS 2..80 */
    Nrf24JamPresetUsb, /* wireless USB dongles */
    Nrf24JamPresetVideo, /* video / FPV (upper ISM) */
    Nrf24JamPresetRc, /* RC controllers (low channels) */
    Nrf24JamPresetZigbee, /* Zigbee ch 11..26 */
    Nrf24JamPresetDrone, /* FHSS drone — random hop 0..124 */
    Nrf24JamPresetCount,
} Nrf24JamPreset;

/* Full display name (e.g. "BLE Advertising"). */
const char* nrf24_jam_preset_name(Nrf24JamPreset preset);

/* Short label for the status view (e.g. "BLE Adv"). */
const char* nrf24_jam_preset_short(Nrf24JamPreset preset);

/* Curated channel list for a preset. Returns NULL with *count = 0 for the
 * Full and Drone presets, which are generated algorithmically. */
const uint8_t* nrf24_jam_preset_channels(Nrf24JamPreset preset, size_t* count);

/* Pick the next channel to jam for this preset, advancing *hop_index.
 * Handles Full (sequential sweep), Drone (random hop) and list-based presets. */
uint8_t nrf24_jam_preset_next_channel(Nrf24JamPreset preset, uint32_t* hop_index);

/* Fill out[] with the full channel set for this preset and return the count.
 * Full and Drone expand to all channels 0..124; list presets copy their list.
 * Used by the jam worker (which then optionally Fisher-Yates shuffles a copy)
 * and by the status view's band visualisation. cap caps the written count. */
size_t nrf24_jam_preset_fill_channels(Nrf24JamPreset preset, uint8_t* out, size_t cap);

/* Sensible default per-channel dwell time in microseconds. Presets with few
 * channels (BLE-Adv, RC, ...) get a long dwell — coverage is cheap, so a clean
 * strong carrier per channel wins. Presets spanning many channels (Full, BT)
 * get a short dwell so the whole band is swept quickly. */
uint16_t nrf24_jam_preset_default_dwell_us(Nrf24JamPreset preset);

/* Default jam strategy (Nrf24JamStrategy) for a preset. Narrow presets that
 * cover only a handful of fixed channels (BLE-Adv 37/38/39, RC, USB, Video) win
 * with a continuous carrier (CW) parked on each channel — maximum spectral
 * energy exactly where it matters. Wide sweeps / FHSS targets default to Turbo
 * (packet collisions + random hop). Returns a Nrf24JamStrategy value. */
uint8_t nrf24_jam_preset_default_strategy(Nrf24JamPreset preset);

/* Default hop order for a preset: sequential (0) for the few-channel CW presets
 * (shuffling 3 channels is pointless), random (1) for the wide Turbo sweeps. */
uint8_t nrf24_jam_preset_default_hop(Nrf24JamPreset preset);

/* Live-tuning bounds / step for the dwell time. */
#define NRF24_JAM_DWELL_MIN_US 100
#define NRF24_JAM_DWELL_MAX_US 5000
#define NRF24_JAM_DWELL_STEP_US 100

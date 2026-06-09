#pragma once

#include <stdint.h>
#include "nrf24_jam_presets.h" /* NRF24_JAM_DWELL_* bounds */
#include "nrf24_channel_source.h" /* Nrf24SourceCount */

/* Per-source jammer configuration (one slot per channel source: Protocol /
 * Manual / WiFi / Activity). Each carries tunable RF parameters which the user
 * edits in the config scene and which persist across reboots. */

typedef enum {
    Nrf24StrategyCw = 0, /* constant carrier (saturates AGC, best vs FHSS) */
    Nrf24StrategyFlood, /* random-garbage packet flooding (collisions) */
    Nrf24StrategyTurbo, /* 3-pattern flood (defeats CRC filters, max pollution) */
    Nrf24StrategyAfh, /* AFH-exhaustion: long CW dwell per channel (dwell = ms),
                       * sequential, to drive adaptive hoppers' channels onto
                       * their bad-channel map. */
    Nrf24StrategyCount,
} Nrf24JamStrategy;

typedef enum {
    Nrf24Pa_Min = 0, /* -18 dBm */
    Nrf24Pa_Low, /* -12 dBm */
    Nrf24Pa_High, /* -6 dBm */
    Nrf24Pa_Max, /*  0 dBm (≈+20 dBm with PA+LNA module) */
    Nrf24Pa_Count,
} Nrf24JamPa;

typedef enum {
    Nrf24Rate_1M = 0,
    Nrf24Rate_2M,
    Nrf24Rate_250K,
    Nrf24Rate_Count,
} Nrf24JamRate;

#define NRF24_JAM_BURST_MIN 1
#define NRF24_JAM_BURST_MAX 20

/* Layout is persisted as a binary blob (saved_struct) — keep field order and
 * types stable, bump NRF24_JAM_CFG_VERSION on any change. */
typedef struct {
    uint8_t pa_level; /* Nrf24JamPa */
    uint8_t data_rate; /* Nrf24JamRate */
    uint8_t strategy; /* Nrf24JamStrategy */
    uint8_t burst_count; /* NRF24_JAM_BURST_MIN..MAX (flood/turbo packets) */
    uint8_t random_hop; /* 0 = sequential sweep, 1 = Fisher-Yates random hop */
    uint16_t dwell_us; /* NRF24_JAM_DWELL_MIN_US..MAX_US per channel */
} Nrf24JamConfig;

/* Load all configs from storage into the in-memory table (defaults if absent
 * or version mismatch). Call once on app start. */
void nrf24_jam_config_load(void);

/* Persist the whole table to storage. */
void nrf24_jam_config_save(void);

/* Restore built-in defaults and persist them. */
void nrf24_jam_config_reset(void);

/* Mutable pointer to the live config for a (source, protocol) pair. The Protocol
 * source keeps a separate slot per preset (BLE-Adv wants CW, Drone wants Turbo,
 * ...), so the preset must be passed; it is ignored for WiFi / Activity. */
Nrf24JamConfig* nrf24_jam_config_get(uint8_t source, uint8_t protocol);

/* Monotonic counter, bumped whenever a config value is edited. The jam worker
 * watches it to know when to re-apply RF settings / rebuild the channel list. */
uint32_t nrf24_jam_config_generation(void);

/* Bump the generation counter (call after editing a config field). */
void nrf24_jam_config_touch(void);

/* Short labels for the status view. */
const char* nrf24_jam_strategy_label(uint8_t strategy); /* "CW"/"FLOOD"/"TURBO" */
const char* nrf24_jam_pa_label(uint8_t pa); /* "MIN".."MAX" */
const char* nrf24_jam_rate_label(uint8_t rate); /* "1M"/"2M"/"250K" */

/* Long labels for the config editor. */
const char* nrf24_jam_strategy_label_long(uint8_t strategy);
const char* nrf24_jam_pa_label_long(uint8_t pa);
const char* nrf24_jam_hop_label(uint8_t random_hop); /* "Sequential"/"Random" */

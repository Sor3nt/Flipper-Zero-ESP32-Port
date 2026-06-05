#include "nrf24_jam_config.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>

#define TAG "Nrf24JamCfg"

#define NRF24_JAM_CFG_PATH    INT_PATH(".nrf24jam.cfg")
#define NRF24_JAM_CFG_MAGIC   (0x4E) /* 'N' — unused by other settings */
#define NRF24_JAM_CFG_VERSION (3) /* v3: 3 sources (Manual removed), was 4 */

/* Persisted blob: one config slot per channel source. */
typedef struct {
    Nrf24JamConfig modes[Nrf24SourceCount];
} Nrf24JamSettings;

static Nrf24JamSettings g_settings;
static uint32_t g_generation = 0;

/* Built-in defaults, one per source. PA max / 2 Mbps / burst 6 throughout;
 * strategy & hop follow the Bruce rationale (CW saturates AGC → good for the
 * WiFi sweep & single channel; Turbo packet collisions + random hop → good for
 * the protocol presets & activity targets). */
static void nrf24_jam_config_defaults(void) {
    static const struct {
        uint8_t strategy;
        uint8_t random_hop;
        uint16_t dwell_us;
    } d[Nrf24SourceCount] = {
        [Nrf24SourceProtocol] = {Nrf24StrategyTurbo, 1, 300},
        [Nrf24SourceWifi] = {Nrf24StrategyCw, 0, 200},
        [Nrf24SourceActivity] = {Nrf24StrategyTurbo, 1, 200},
    };

    for(uint32_t i = 0; i < Nrf24SourceCount; i++) {
        g_settings.modes[i].pa_level = Nrf24Pa_Max;
        g_settings.modes[i].data_rate = Nrf24Rate_2M;
        g_settings.modes[i].strategy = d[i].strategy;
        g_settings.modes[i].burst_count = 6;
        g_settings.modes[i].random_hop = d[i].random_hop;
        g_settings.modes[i].dwell_us = d[i].dwell_us;
    }
}

static void nrf24_jam_config_clamp(Nrf24JamConfig* c) {
    if(c->pa_level >= Nrf24Pa_Count) c->pa_level = Nrf24Pa_Max;
    if(c->data_rate >= Nrf24Rate_Count) c->data_rate = Nrf24Rate_2M;
    if(c->strategy >= Nrf24StrategyCount) c->strategy = Nrf24StrategyCw;
    if(c->burst_count < NRF24_JAM_BURST_MIN || c->burst_count > NRF24_JAM_BURST_MAX)
        c->burst_count = 6;
    if(c->random_hop > 1) c->random_hop = 0;
    if(c->dwell_us < NRF24_JAM_DWELL_MIN_US) c->dwell_us = NRF24_JAM_DWELL_MIN_US;
    if(c->dwell_us > NRF24_JAM_DWELL_MAX_US) c->dwell_us = NRF24_JAM_DWELL_MAX_US;
}

void nrf24_jam_config_load(void) {
    nrf24_jam_config_defaults();

    if(!saved_struct_load(
           NRF24_JAM_CFG_PATH,
           &g_settings,
           sizeof(Nrf24JamSettings),
           NRF24_JAM_CFG_MAGIC,
           NRF24_JAM_CFG_VERSION)) {
        FURI_LOG_D(TAG, "No saved config, using defaults");
        nrf24_jam_config_defaults();
    }

    for(uint32_t i = 0; i < Nrf24SourceCount; i++) {
        nrf24_jam_config_clamp(&g_settings.modes[i]);
    }
    g_generation++;
}

void nrf24_jam_config_save(void) {
    if(!saved_struct_save(
           NRF24_JAM_CFG_PATH,
           &g_settings,
           sizeof(Nrf24JamSettings),
           NRF24_JAM_CFG_MAGIC,
           NRF24_JAM_CFG_VERSION)) {
        FURI_LOG_E(TAG, "Failed to save config");
    }
}

void nrf24_jam_config_reset(void) {
    nrf24_jam_config_defaults();
    g_generation++;
    nrf24_jam_config_save();
}

Nrf24JamConfig* nrf24_jam_config_get(uint8_t source) {
    if(source >= Nrf24SourceCount) source = 0;
    return &g_settings.modes[source];
}

uint32_t nrf24_jam_config_generation(void) {
    return g_generation;
}

void nrf24_jam_config_touch(void) {
    g_generation++;
}

const char* nrf24_jam_strategy_label(uint8_t strategy) {
    switch(strategy) {
    case Nrf24StrategyCw:
        return "CW";
    case Nrf24StrategyFlood:
        return "FLOOD";
    case Nrf24StrategyTurbo:
        return "TURBO";
    case Nrf24StrategyAfh:
        return "AFH";
    default:
        return "?";
    }
}

const char* nrf24_jam_strategy_label_long(uint8_t strategy) {
    switch(strategy) {
    case Nrf24StrategyCw:
        return "Const Carrier";
    case Nrf24StrategyFlood:
        return "Data Flooding";
    case Nrf24StrategyTurbo:
        return "Turbo Flood";
    case Nrf24StrategyAfh:
        return "AFH Exhaust";
    default:
        return "?";
    }
}

const char* nrf24_jam_pa_label(uint8_t pa) {
    switch(pa) {
    case Nrf24Pa_Min:
        return "MIN";
    case Nrf24Pa_Low:
        return "LOW";
    case Nrf24Pa_High:
        return "HIGH";
    case Nrf24Pa_Max:
        return "MAX";
    default:
        return "?";
    }
}

const char* nrf24_jam_pa_label_long(uint8_t pa) {
    switch(pa) {
    case Nrf24Pa_Min:
        return "MIN -18dBm";
    case Nrf24Pa_Low:
        return "LOW -12dBm";
    case Nrf24Pa_High:
        return "HIGH -6dBm";
    case Nrf24Pa_Max:
        return "MAX 0dBm";
    default:
        return "?";
    }
}

const char* nrf24_jam_rate_label(uint8_t rate) {
    switch(rate) {
    case Nrf24Rate_1M:
        return "1M";
    case Nrf24Rate_2M:
        return "2M";
    case Nrf24Rate_250K:
        return "250K";
    default:
        return "?";
    }
}

const char* nrf24_jam_hop_label(uint8_t random_hop) {
    return random_hop ? "Random" : "Sequential";
}

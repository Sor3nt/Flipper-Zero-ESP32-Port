#include "config.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <string.h>

/* The /data prefix resolves to this app's own data directory
 * (/ext/apps_data/morse_beacon), created by the storage service on first
 * use - the catalog-sanctioned place for an app to keep its files. */
#define MORSE_CONFIG_PATH APP_DATA_PATH("beacon.conf")

void morse_config_set_defaults(MorseConfig* config) {
    memset(config, 0, sizeof(MorseConfig));
    strncpy(config->id_text, "DE CALLSIGN", MORSE_MAX_TEXT);

    // 70cm / ISM, the band the Flipper's antenna is matched for. Deliberately
    // not a GMRS channel: a settings reset must never drop the app onto a band
    // that needs a licence and type-accepted equipment to use.
    config->frequency = 433920000;
    config->mode = CwModeMcw;
    config->deviation = CwDeviationNarrow;
    config->rx_bw = CwRxBw101;
    config->tone_hz = 800;
    config->wpm = 18;
    config->farnsworth = 0;
    config->preamble_ms = 300;
    config->tail_ms = 200;
    config->external_radio = false;

    config->squelch_dbm = -85;
    config->quiet_time_s = 600; // 10 min of dead air arms the ID
    config->max_interval_s = 900; // ...and never go longer than 15 min
    config->courtesy_delay_ms = 1500;
    config->min_carrier_ms = 250;
    config->hang_ms = 600;
    config->beacon_trigger = BeaconTriggerActivity;
    config->period_s = 600; // 10 min - the Part 97 station ID interval
    // Start to start by default: a beacon is only useful if its cadence is
    // predictable, and end-to-end quietly folds the length of every
    // transmission into the period.
    config->interval_anchor = IntervalAnchorStart;
    config->id_on_start = false;

    config->link_source = LinkSourceBle;
    config->link_baudrate = 9600;
}

static uint32_t clamp_u32(uint32_t value, uint32_t min, uint32_t max) {
    if(value < min) return min;
    if(value > max) return max;
    return value;
}

void morse_config_validate(MorseConfig* config) {
    config->id_text[MORSE_MAX_TEXT] = '\0';

    /* Fall back to the default when the saved frequency is one the radio
     * cannot tune, or one the firmware's region policy will not permit -
     * a settings file can arrive from a different firmware or region. The
     * region table is still the firmware's own; 433.92 is in every one. */
    if(!cw_radio_frequency_supported(config->frequency) ||
       !cw_radio_tx_allowed(config->frequency)) {
        config->frequency = 433920000;
    }
    if(config->mode >= CwModeCount) config->mode = CwModeMcw;
    if(config->deviation >= CwDeviationCount) config->deviation = CwDeviationNarrow;
    if(config->rx_bw >= CwRxBwCount) config->rx_bw = CwRxBw101;
    if(config->link_source >= LinkSourceCount) config->link_source = LinkSourceBle;

    // snap to the step each settings row iterates over, then bound it
    config->tone_hz = 400 + clamp_u32((config->tone_hz - 400) / 50, 0, 16) * 50;
    config->wpm = 5 + clamp_u32((uint32_t)config->wpm - 5, 0, 35);
    if(config->farnsworth && (config->farnsworth < 5 || config->farnsworth > 20)) {
        config->farnsworth = 0;
    }
    config->squelch_dbm = -110 + (int8_t)(clamp_u32((uint32_t)(config->squelch_dbm + 110), 0, 60) / 5 * 5);
    config->courtesy_delay_ms = clamp_u32(config->courtesy_delay_ms, 0, 5000) / 500 * 500;
    config->preamble_ms = clamp_u32(config->preamble_ms, 0, 1000) / 100 * 100;
    config->tail_ms = clamp_u32(config->tail_ms, 0, 1000) / 100 * 100;

    bool baud_ok = false;
    static const uint32_t baud_allowed[] = {9600, 19200, 38400, 57600, 115200};
    for(size_t i = 0; i < COUNT_OF(baud_allowed); i++) {
        if(config->link_baudrate == baud_allowed[i]) baud_ok = true;
    }
    if(!baud_ok) config->link_baudrate = 9600;

    bool quiet_ok = false;
    static const uint16_t quiet_allowed[] = {30, 60, 120, 180, 300, 600, 900, 1200, 1800};
    for(size_t i = 0; i < COUNT_OF(quiet_allowed); i++) {
        if(config->quiet_time_s == quiet_allowed[i]) quiet_ok = true;
    }
    if(!quiet_ok) config->quiet_time_s = 600;

    if(config->beacon_trigger >= BeaconTriggerCount) {
        config->beacon_trigger = BeaconTriggerActivity;
    }
    if(config->interval_anchor >= IntervalAnchorCount) {
        config->interval_anchor = IntervalAnchorStart;
    }
    bool period_ok = false;
    static const uint16_t period_allowed[] =
        {15, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800};
    for(size_t i = 0; i < COUNT_OF(period_allowed); i++) {
        if(config->period_s == period_allowed[i]) period_ok = true;
    }
    if(!period_ok) config->period_s = 600;

    bool interval_ok = false;
    static const uint16_t interval_allowed[] = {0, 300, 600, 900, 1200, 1800, 3600};
    for(size_t i = 0; i < COUNT_OF(interval_allowed); i++) {
        if(config->max_interval_s == interval_allowed[i]) interval_ok = true;
    }
    if(!interval_ok) config->max_interval_s = 900;

    config->min_carrier_ms = clamp_u32(config->min_carrier_ms, 20, 5000);
    config->hang_ms = clamp_u32(config->hang_ms, 20, 5000);
}

void morse_config_load(MorseConfig* config) {
    morse_config_set_defaults(config);

    MorseConfig loaded;
    memset(&loaded, 0, sizeof(MorseConfig));
    if(saved_struct_load(
           MORSE_CONFIG_PATH,
           &loaded,
           sizeof(MorseConfig),
           MORSE_CONFIG_MAGIC,
           MORSE_CONFIG_VERSION)) {
        *config = loaded;
        morse_config_validate(config);
    }
}

void morse_config_save(const MorseConfig* config) {
    saved_struct_save(
        MORSE_CONFIG_PATH,
        (void*)config,
        sizeof(MorseConfig),
        MORSE_CONFIG_MAGIC,
        MORSE_CONFIG_VERSION);
}

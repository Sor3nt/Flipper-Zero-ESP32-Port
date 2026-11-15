#pragma once

#include "morse.h"
#include "radio.h"
#include "link.h"

#define MORSE_CONFIG_VERSION 4
#define MORSE_CONFIG_MAGIC   0x4D // 'M'

typedef enum {
    BeaconTriggerActivity, // ID after traffic, once the channel has been quiet
    BeaconTriggerInterval, // ID every `period_s`, regardless of the channel
    BeaconTriggerBoth,
    BeaconTriggerCount,
} BeaconTrigger;

typedef enum {
    /* Measure the interval from the end of one transmission to the start of the
     * next, so the gap of dead air is what stays constant. The cadence then
     * includes the length of the identifier and walks with it. */
    IntervalAnchorEnd,
    /* Measure start to start: transmissions begin at fixed multiples of the
     * period, which is what a hunt needs. If the identifier is longer than the
     * period there is no way to honour that without keying continuously, so
     * whole slots are skipped and the overrun is logged. */
    IntervalAnchorStart,
    IntervalAnchorCount,
} IntervalAnchor;

typedef struct {
    char id_text[MORSE_MAX_TEXT + 1];

    uint32_t frequency;
    uint8_t mode; // CwMode
    uint8_t deviation; // CwDeviation
    uint8_t rx_bw; // CwRxBw
    uint16_t tone_hz;
    uint8_t wpm;
    uint8_t farnsworth; // 0 = off
    uint16_t preamble_ms;
    uint16_t tail_ms;
    bool external_radio;

    int8_t squelch_dbm;
    uint16_t quiet_time_s; // channel must be this quiet to arm the ID
    uint16_t max_interval_s; // force an ID this long after the last one (0 = off)
    uint16_t courtesy_delay_ms; // wait after the channel clears before keying
    uint16_t min_carrier_ms; // carrier must persist this long to count as traffic
    uint16_t hang_ms; // carrier must be gone this long to count as clear
    uint8_t beacon_trigger; // BeaconTrigger
    uint16_t period_s; // interval-mode period
    uint8_t interval_anchor; // IntervalAnchor - what the period is measured between
    bool id_on_start;

    uint8_t link_source; // LinkSource
    uint32_t link_baudrate;
} MorseConfig;

void morse_config_set_defaults(MorseConfig* config);

/** Clamp every field into range. A settings file written by a different build
 *  must never be able to index off the end of a value table. */
void morse_config_validate(MorseConfig* config);
void morse_config_load(MorseConfig* config);
void morse_config_save(const MorseConfig* config);

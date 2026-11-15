#pragma once

#include <lib/subghz/devices/devices.h>
#include "morse.h"

typedef enum {
    CwModeMcw, // FM carrier with an audible tone keyed on/off - what an FM rig hears
    CwModeOok, // carrier keyed on/off - true CW, silent on an FM rig
    CwModeSsbUsb, // OOK with the carrier tone_hz ABOVE the dial - USB rigs hear the tone
    CwModeSsbLsb, // OOK with the carrier tone_hz BELOW the dial - LSB rigs hear the tone
    CwModeCount,
} CwMode;

typedef enum {
    CwDeviationNarrow, // +/-2.4 kHz, 12.5 kHz channels
    CwDeviationWide, // +/-4.8 kHz, 25 kHz channels
    CwDeviationCount,
} CwDeviation;

typedef enum {
    CwRxBw58, // tightest - best adjacent-channel rejection
    CwRxBw101,
    CwRxBw135,
    CwRxBw270, // CC1101 stock
    CwRxBwCount,
} CwRxBw;

typedef struct {
    uint32_t frequency;
    CwMode mode;
    CwDeviation deviation;
    CwRxBw rx_bw;
    uint16_t tone_hz;
} CwRadioParams;

typedef struct CwRadio CwRadio;

CwRadio* cw_radio_alloc(void);
void cw_radio_free(CwRadio* radio);

/** Take the radio. `external` selects an attached CC1101 module over the internal one. */
bool cw_radio_open(CwRadio* radio, bool external);
void cw_radio_close(CwRadio* radio);
bool cw_radio_is_open(const CwRadio* radio);
bool cw_radio_is_external(const CwRadio* radio);

/** True if the radio can be tuned here at all.
 *
 * Checked in the app rather than by calling subghz_devices_is_frequency_valid():
 * the internal CC1101's implementation calls furi_crash() on a bad frequency
 * instead of returning false, so asking it is not a safe test. These are the
 * firmware's own hardware ranges - no region table is involved, and none is
 * imposed here. Whether TX is then permitted is left to the firmware. */
bool cw_radio_frequency_supported(uint32_t frequency);

bool cw_radio_frequency_valid(CwRadio* radio, uint32_t frequency);

/** Whether the running firmware permits transmitting here. Firmware policy,
 *  not ours - it varies by build and by provisioned region. */
bool cw_radio_tx_allowed(uint32_t frequency);

/** Park in RX so the channel can be watched. Safe to call repeatedly. */
bool cw_radio_listen(CwRadio* radio, const CwRadioParams* params);
float cw_radio_rssi(CwRadio* radio);
void cw_radio_idle(CwRadio* radio);

/** Blocking transmit of a rendered stream. Leaves the radio idle. */
bool cw_radio_send(CwRadio* radio, const MorseStream* stream, const CwRadioParams* params);

/** Cut a transmission short at the next element boundary. */
void cw_radio_abort(CwRadio* radio);

/** 0..100 progress of the transmission in flight, for the UI thread. */
uint8_t cw_radio_progress(const CwRadio* radio);
bool cw_radio_is_sending(const CwRadio* radio);

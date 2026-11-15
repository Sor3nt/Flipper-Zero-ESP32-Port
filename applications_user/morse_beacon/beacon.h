#pragma once

#include "config.h"
#include "radio.h"

typedef enum {
    BeaconStateStopped,
    BeaconStateListening,
    BeaconStateBusy, // someone is on the channel
    BeaconStatePending, // channel clear, courtesy delay running
    BeaconStateSending,
    BeaconStateError,
} BeaconState;

typedef struct {
    BeaconState state;
    float rssi;
    bool carrier;
    bool armed; // the next transmission will be followed by an ID
    uint32_t quiet_ms; // how long the channel has been clear
    uint32_t since_id_ms;
    uint32_t next_id_ms; // countdown to the next scheduled ID (interval modes)
    uint32_t id_count;
    uint32_t traffic_count;
    uint8_t progress;
} BeaconStatus;

typedef struct Beacon Beacon;

Beacon* beacon_alloc(CwRadio* radio);
void beacon_free(Beacon* beacon);

void beacon_start(Beacon* beacon, const MorseConfig* config);
void beacon_stop(Beacon* beacon);
bool beacon_is_running(Beacon* beacon);

/** Key an ID at the next opportunity, regardless of timers. */
void beacon_request_id(Beacon* beacon);

void beacon_get_status(Beacon* beacon, BeaconStatus* out);

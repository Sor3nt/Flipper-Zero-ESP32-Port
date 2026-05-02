#pragma once

#include <gui/view.h>
#include <esp_gap_ble_api.h>

#define RACE_DETECTOR_MAX_DEVICES    24
#define RACE_DETECTOR_ITEMS_ON_SCREEN 4

typedef enum {
    RaceStatusUnknown = 0,
    RaceStatusProbing,
    RaceStatusVulnerable,
    RaceStatusClean,
    RaceStatusConnectFail,
} RaceStatus;

typedef struct {
    esp_bd_addr_t addr;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    char name[24];
    RaceStatus status;
} RaceDevice;

typedef struct {
    RaceDevice devices[RACE_DETECTOR_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    bool probing;
} RaceDetectorModel;

View* race_detector_view_alloc(void);
void race_detector_view_free(View* view);

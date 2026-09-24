#pragma once
#include "wardriver_config.h"
#include "wardriver_nmea.h"
typedef struct {
    WardriverNmea parser;
    bool installed;
    int uart, rx, tx;
    char status[24];
} WardriverGps;
void wardriver_gps_start(WardriverGps* gps,const WardriverConfig* config);
void wardriver_gps_poll(WardriverGps* gps,uint32_t now_ms);
void wardriver_gps_stop(WardriverGps* gps);

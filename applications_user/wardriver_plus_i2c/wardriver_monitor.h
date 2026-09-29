#pragma once
#include "wardriver_detect.h"
#define WARD_AP_WATCH 64
#define WARD_BURST_WATCH 16
typedef struct {
    bool used, encrypted, alerted;
    uint8_t mac[6]; char ssid[33]; uint32_t seen, last_alert;
} WardriverApWatch;
typedef struct {
    bool used, alerted; uint8_t mac[6]; uint16_t sequence;
    unsigned count; uint32_t window, seen, last_alert;
} WardriverBurstWatch;
typedef struct {
    WardriverApWatch aps[WARD_AP_WATCH];
    WardriverBurstWatch bursts[WARD_BURST_WATCH];
} WardriverMonitor;
/* Wi-Fi callback owns this bounded state; reset on every session. No allocation. */
bool wardriver_monitor_frame(WardriverMonitor* monitor,const uint8_t* frame,size_t length,uint32_t now,WardriverObservation* out);

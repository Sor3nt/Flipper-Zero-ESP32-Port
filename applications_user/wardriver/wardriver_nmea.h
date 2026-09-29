#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool enabled, connected, has_fix;
    bool altitude_valid, hdop_valid, satellites_valid, timestamp_valid;
    double latitude, longitude, altitude;
    float hdop;
    uint8_t satellites;
    uint64_t timestamp; /* UTC Unix seconds, never the local timezone. */
} WardriverGpsData;

typedef struct {
    WardriverGpsData data;
    char line[160];
    size_t length;
    bool collecting, overflow, date_valid, rmc_fix, gga_fix, seen_rmc, seen_gga;
    uint64_t day_epoch;
    uint32_t last_sentence, last_rmc, last_gga, rmc_second, gga_second;
} WardriverNmea;

void wardriver_nmea_init(WardriverNmea* parser);
void wardriver_nmea_feed(WardriverNmea* parser, const void* data, size_t length, uint32_t now_ms);
WardriverGpsData wardriver_nmea_snapshot(const WardriverNmea* parser, uint32_t now_ms);
void wardriver_format_time(uint64_t timestamp, char out[20]);

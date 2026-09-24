#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BW16_LINE_SIZE 128
#define BW16_MAX_APS 50
#define BW16_SCAN_TIMEOUT_MS 20000U

typedef struct {
    char ssid[33];
    char mac[18];
    unsigned channel;
    int rssi;
} Bw16Ap;
typedef enum { Bw16Ignore, Bw16Record, Bw16Done, Bw16Empty, Bw16Malformed } Bw16LineKind;
typedef struct { char text[BW16_LINE_SIZE]; size_t used; bool discard; } Bw16Line;
typedef enum { Bw16More, Bw16LineReady, Bw16LineDropped } Bw16Frame;
Bw16LineKind bw16_parse(const char* line, Bw16Ap* ap);
Bw16Frame bw16_frame(Bw16Line* line, uint8_t byte);
bool bw16_scan_expired(uint32_t now, uint32_t start);

#pragma once
#include <furi.h>
#include <storage/storage.h>
#include "../../firmware/bw16_controller/bw16_pcap.h"

#define BW16_CAPTURE_PACKETS 256U
#define BW16_CAPTURE_DIRECTORY "/ext/apps_data/bw16/captures"
typedef enum {
    Bw16CaptureNone, Bw16CaptureControl, Bw16CapturePacket,
    Bw16CaptureDropped, Bw16CaptureWriteError, Bw16CaptureLimit
} Bw16CaptureEvent;
typedef struct {
    Storage* storage;
    File* file;
    bool opened, created, failed, closed, discard, have_sequence;
    unsigned used, packets, eapol;
    uint32_t session, anchor, sequence, usec, dropped, gaps;
    uint8_t bssid[6];
    char path[80], error[48];
    char line[BWC_LINE_SIZE];
    BwcPacket packet;
} Bw16Capture;

/* Preflight creates a new file and syncs its PCAP header before CAPTURE is sent. */
bool bw16_capture_prepare(Bw16Capture* capture, const char* bssid, uint32_t session, uint32_t anchor);
bool bw16_capture_close(Bw16Capture* capture);
Bw16CaptureEvent bw16_capture_byte(Bw16Capture* capture, uint8_t byte, bool start_confirmed);

/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BW16_NATIVE_H
#define BW16_NATIVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BWN_MAX_APS 50U
#define BWN_MAX_CLIENTS 32U
#define BWN_LINE_SIZE 80U
#define BWN_OPERATION_MS 30000U
#define BWN_VISIT_MS 100U
#define BWN_CAPTURE_MS 120000U
#define BWN_CAPTURE_PACKETS 256U
#define BWN_CAPTURE_DEAUTH_MS 1000U
typedef struct { char ssid[33]; uint8_t mac[6], channel; int16_t rssi; } BwnAp;
typedef enum { BwnIdle, BwnScan, BwnObserve, BwnStation, BwnClient, BwnAll, BwnSelected, BwnCapture } BwnState;
typedef struct {
    void (*emit)(void* context, const char* line); /* complete line without newline */
    bool (*scan_begin)(void* context);
    bool (*observe_begin)(void* context, const BwnAp* ap);
    void (*observe_end)(void* context);
    bool (*set_channel)(void* context, uint8_t channel);
    void (*transmit)(void* context, const uint8_t source[6], const uint8_t dest[6], uint16_t reason);
    bool (*capture_begin)(void* context, const BwnAp* ap, uint32_t session);
    void (*capture_end)(void* context); /* Stop RX, drain queued records, update progress. */
    unsigned (*capture_deauth)(void* context, const BwnAp* ap); /* One armed burst; calls attempted. */
} BwnIo;
typedef struct {
    BwnIo io; void* context; BwnState state;
    BwnAp aps[BWN_MAX_APS], targets[BWN_MAX_APS];
    uint8_t target_indices[BWN_MAX_APS], clients[BWN_MAX_CLIENTS][6], destination[6];
    unsigned ap_count, target_count, position, client_count, client_ap;
    bool scan_valid, indices_valid, clients_valid, list_after_scan;
    bool rx_discard, channel_ready, all_waiting, visit_sent;
    uint32_t started, visit_started, report_started, rx_started, counter, cycles;
    uint32_t capture_session, capture_emitted, capture_dropped;
    bool capture_complete, capture_with_deauth, capture_deauth_pending;
    char rx[BWN_LINE_SIZE]; unsigned rx_used;
} BwnController;
void bwn_init(BwnController*, const BwnIo*, void* context);
void bwn_command(BwnController*, const char* line, uint32_t now);
void bwn_feed(BwnController*, uint8_t byte, uint32_t now);
void bwn_tick(BwnController*, uint32_t now);
void bwn_stop(BwnController*);
/* Call these only on the same thread as bwn_tick, after copying callback data. */
void bwn_scan_result(BwnController*, const BwnAp* ap);
void bwn_scan_complete(BwnController*, bool success);
void bwn_client_result(BwnController*, const uint8_t mac[6]);
void bwn_capture_progress(BwnController*, uint32_t emitted, uint32_t dropped, bool complete);
bool bwn_client_from_frame(const uint8_t* frame, size_t length, const uint8_t bssid[6], uint8_t client[6]);
#ifdef __cplusplus
}
#endif
#endif

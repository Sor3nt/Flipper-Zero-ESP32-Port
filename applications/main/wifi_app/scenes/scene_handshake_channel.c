#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_pcap.h"
#include "../wifi_handshake_parser.h"
#include "../views/handshake_channel_view.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <storage/storage.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

// ---------------------------------------------------------------------------
// Ring buffer (same pattern as scene_handshake.c)
// ---------------------------------------------------------------------------
#define HSC_PKT_POOL_SIZE 32
#define HSC_PKT_MAX_LEN  512

typedef struct {
    uint16_t len;
    uint32_t timestamp_us;
    uint8_t data[HSC_PKT_MAX_LEN];
} HscPkt;

static HscPkt* s_hsc_pkt_pool = NULL;
static volatile uint32_t s_hsc_write_idx = 0;
static volatile uint32_t s_hsc_read_idx = 0;

// ---------------------------------------------------------------------------
// Multi-target handshake state
// ---------------------------------------------------------------------------
#define HSC_MAX_TARGETS 16

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool has_beacon;
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
    File* pcap_file; // per-target PCAP
} HscTarget;

static HscTarget s_targets[HSC_MAX_TARGETS];
static uint8_t s_target_count = 0;
static Storage* s_hsc_storage = NULL;
static uint8_t s_hsc_channel = 11;

// ---------------------------------------------------------------------------
// Target management
// ---------------------------------------------------------------------------

static HscTarget* hsc_find_target(const uint8_t* bssid) {
    for(int i = 0; i < s_target_count; i++) {
        if(memcmp(s_targets[i].bssid, bssid, 6) == 0) return &s_targets[i];
    }
    return NULL;
}

static HscTarget* hsc_find_or_create_target(const uint8_t* bssid) {
    HscTarget* t = hsc_find_target(bssid);
    if(t) return t;
    if(s_target_count >= HSC_MAX_TARGETS) return NULL;

    t = &s_targets[s_target_count++];
    memset(t, 0, sizeof(HscTarget));
    memcpy(t->bssid, bssid, 6);
    return t;
}

static bool hsc_is_complete(HscTarget* t) {
    return t->has_m2 && t->has_m3;
}

// ---------------------------------------------------------------------------
// SSID sanitization for filename
// ---------------------------------------------------------------------------
static void hsc_sanitize_ssid(const char* ssid, const uint8_t* bssid, char* out, size_t out_len) {
    if(ssid[0] == '\0') {
        snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        return;
    }
    size_t j = 0;
    for(size_t i = 0; ssid[i] && j < out_len - 1; i++) {
        char c = ssid[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

// ---------------------------------------------------------------------------
// Ensure PCAP file for target
// ---------------------------------------------------------------------------
static void hsc_ensure_pcap(HscTarget* t) {
    if(t->pcap_file || !s_hsc_storage) return;

    storage_common_mkdir(s_hsc_storage, "/ext/wifi");
    storage_common_mkdir(s_hsc_storage, "/ext/wifi/handshakes");

    char safe_ssid[64];
    hsc_sanitize_ssid(t->ssid, t->bssid, safe_ssid, sizeof(safe_ssid));

    char path[128];
    snprintf(path, sizeof(path), "/ext/wifi/handshakes/%s.pcap", safe_ssid);
    if(!storage_common_stat(s_hsc_storage, path, NULL)) {
        for(int i = 1; i < 999; i++) {
            snprintf(path, sizeof(path), "/ext/wifi/handshakes/%s_%d.pcap", safe_ssid, i);
            if(storage_common_stat(s_hsc_storage, path, NULL)) break;
        }
    }
    t->pcap_file = wifi_pcap_open(s_hsc_storage, path);
}

// ---------------------------------------------------------------------------
// Process a single packet for multi-target capture
// ---------------------------------------------------------------------------
static void hsc_process_packet(const uint8_t* payload, int len) {
    if(len < 24) return;

    // Beacon — extract BSSID and SSID
    if(hs_is_beacon(payload, len)) {
        const uint8_t* bssid = &payload[16];
        HscTarget* t = hsc_find_or_create_target(bssid);
        if(!t) return;

        if(!t->has_beacon) {
            t->has_beacon = true;
            // Extract SSID from beacon tagged parameters
            if(t->ssid[0] == '\0') {
                hs_extract_beacon_ssid(payload, len, t->ssid, sizeof(t->ssid));
            }
        }
        // Write beacon to PCAP
        // (defer PCAP open until we have EAPOL — beacons alone aren't useful)
        return;
    }

    // Data frame?
    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    if(frame_type != 2) return;

    const uint8_t* bssid = NULL;
    const uint8_t* station = NULL;
    const uint8_t* ap = NULL;
    int header_len = 0;
    if(!hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len)) return;
    if(!hs_is_eapol(payload, header_len, len)) return;

    const uint8_t* llc = &payload[header_len];
    const uint8_t* eapol_start = &llc[8];
    int eapol_len = len - header_len - 8;

    uint8_t msg_num = hs_get_eapol_msg_num(eapol_start, eapol_len);
    if(msg_num == 0) return;

    HscTarget* t = hsc_find_or_create_target(bssid);
    if(!t) return;

    ESP_LOGI(TAG, "Ch%d EAPOL M%d for %02X:%02X:%02X:%02X:%02X:%02X",
             s_hsc_channel, msg_num,
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    switch(msg_num) {
    case 1: t->has_m1 = true; break;
    case 2: t->has_m2 = true; break;
    case 3: t->has_m3 = true; break;
    case 4: t->has_m4 = true; break;
    }

    // Open PCAP on first EAPOL and write the packet
    hsc_ensure_pcap(t);
    if(t->pcap_file) {
        wifi_pcap_write_packet(t->pcap_file, 0, payload, len);
    }
}

// ---------------------------------------------------------------------------
// Promiscuous callback — accepts ALL beacons + EAPOL (no BSSID filter)
// ---------------------------------------------------------------------------
static void hsc_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if(len < 24 || len > HSC_PKT_MAX_LEN) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;

    // Beacon
    if(frame_type == 0 && frame_subtype == 8) {
        // Only enqueue if we have room and < max targets
        if(s_target_count >= HSC_MAX_TARGETS) {
            // Check if this BSSID is already known
            bool known = false;
            for(int i = 0; i < s_target_count; i++) {
                if(memcmp(s_targets[i].bssid, &payload[16], 6) == 0) {
                    known = true;
                    break;
                }
            }
            if(!known) return; // drop — target list full
        }
        // Fall through to enqueue
    }
    // Data frame — quick EAPOL check
    else if(frame_type == 2) {
        int hdr_len = 24;
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) hdr_len = 30;
        if((frame_subtype & 0x08) == 0x08) hdr_len += 2;
        if(len < hdr_len + 8) return;
        const uint8_t* llc = &payload[hdr_len];
        if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;
    }
    else {
        return;
    }

    // Ring buffer enqueue
    uint32_t next = (s_hsc_write_idx + 1) % HSC_PKT_POOL_SIZE;
    if(next == s_hsc_read_idx) return;

    HscPkt* slot = &s_hsc_pkt_pool[s_hsc_write_idx];
    memcpy(slot->data, payload, len);
    slot->len = len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    s_hsc_write_idx = next;
}

// ---------------------------------------------------------------------------
// Drain ring buffer
// ---------------------------------------------------------------------------
static void hsc_drain_and_process(void) {
    while(s_hsc_read_idx != s_hsc_write_idx) {
        HscPkt* pkt = &s_hsc_pkt_pool[s_hsc_read_idx];
        hsc_process_packet(pkt->data, pkt->len);
        s_hsc_read_idx = (s_hsc_read_idx + 1) % HSC_PKT_POOL_SIZE;
    }
}

// ---------------------------------------------------------------------------
// Update view model from target state
// ---------------------------------------------------------------------------
static void hsc_update_view(WifiApp* app) {
    HsChannelViewModel* model = view_get_model(app->view_handshake_channel);
    model->channel = s_hsc_channel;
    model->running = true;
    model->count = s_target_count;

    // Count completed handshakes
    uint8_t hs_done = 0;
    for(int i = 0; i < s_target_count; i++) {
        if(hsc_is_complete(&s_targets[i])) hs_done++;
    }
    model->hs_complete_count = hs_done;

    for(int i = 0; i < s_target_count && i < HS_CHANNEL_VIEW_MAX; i++) {
        HscTarget* t = &s_targets[i];
        HsChannelEntry* e = &model->entries[i];
        // Copy max 10 chars of SSID
        strncpy(e->ssid, t->ssid[0] ? t->ssid : "?", 10);
        e->ssid[10] = '\0';
        e->has_m1 = t->has_m1;
        e->has_m2 = t->has_m2;
        e->has_m3 = t->has_m3;
        e->has_m4 = t->has_m4;
        e->has_beacon = t->has_beacon;
        e->complete = hsc_is_complete(t);
    }

    view_commit_model(app->view_handshake_channel, true);
}

// ---------------------------------------------------------------------------
// Scene handlers
// ---------------------------------------------------------------------------

void wifi_app_scene_handshake_channel_on_enter(void* context) {
    WifiApp* app = context;

    // Reset state
    s_hsc_write_idx = 0;
    s_hsc_read_idx = 0;
    s_target_count = 0;
    s_hsc_channel = 11;
    memset(s_targets, 0, sizeof(s_targets));

    // Allocate ring buffer
    if(!s_hsc_pkt_pool) {
        s_hsc_pkt_pool = malloc(sizeof(HscPkt) * HSC_PKT_POOL_SIZE);
    }

    // Start WiFi
    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }

    // Set channel and start promiscuous
    wifi_hal_set_promiscuous(true, NULL);
    wifi_hal_set_channel(s_hsc_channel);
    wifi_hal_set_promiscuous(true, hsc_rx_callback);

    // Open storage
    s_hsc_storage = furi_record_open(RECORD_STORAGE);

    // Init view
    HsChannelViewModel* model = view_get_model(app->view_handshake_channel);
    memset(model, 0, sizeof(HsChannelViewModel));
    model->channel = s_hsc_channel;
    model->running = true;
    view_commit_model(app->view_handshake_channel, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewHandshakeChannel);

    ESP_LOGI(TAG, "Handshake channel capture started on ch%d", s_hsc_channel);
}

bool wifi_app_scene_handshake_channel_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyUp) {
            if(s_hsc_channel < 13) s_hsc_channel++; else s_hsc_channel = 1;
            wifi_hal_set_promiscuous(false, NULL);
            wifi_hal_set_channel(s_hsc_channel);
            furi_delay_ms(20);
            wifi_hal_set_promiscuous(true, hsc_rx_callback);
            ESP_LOGI(TAG, "Channel changed to %d", s_hsc_channel);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            if(s_hsc_channel > 1) s_hsc_channel--; else s_hsc_channel = 13;
            wifi_hal_set_promiscuous(false, NULL);
            wifi_hal_set_channel(s_hsc_channel);
            furi_delay_ms(20);
            wifi_hal_set_promiscuous(true, hsc_rx_callback);
            ESP_LOGI(TAG, "Channel changed to %d", s_hsc_channel);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        hsc_drain_and_process();
        hsc_update_view(app);
    }

    return consumed;
}

void wifi_app_scene_handshake_channel_on_exit(void* context) {
    UNUSED(context);

    // Stop promiscuous
    wifi_hal_set_promiscuous(false, NULL);

    // Close all PCAP files
    for(int i = 0; i < s_target_count; i++) {
        if(s_targets[i].pcap_file) {
            wifi_pcap_close(s_targets[i].pcap_file);
            s_targets[i].pcap_file = NULL;
        }
    }

    // Close storage
    if(s_hsc_storage) {
        furi_record_close(RECORD_STORAGE);
        s_hsc_storage = NULL;
    }

    // Free ring buffer
    if(s_hsc_pkt_pool) {
        free(s_hsc_pkt_pool);
        s_hsc_pkt_pool = NULL;
    }

    s_target_count = 0;
}

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
#define HSC_PKT_POOL_SIZE 64
#define HSC_PKT_MAX_LEN  512
#define HSC_DRAIN_MAX_PER_TICK 32

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
// Cached beacon to flush into the PCAP once it gets opened on first EAPOL.
#define HSC_BEACON_CACHE_LEN 384

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool has_beacon;
    bool ssid_resolved; // true once we've seen a non-hidden SSID tag
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
    File* pcap_file;
    uint8_t cached_beacon[HSC_BEACON_CACHE_LEN];
    uint16_t cached_beacon_len;
    uint32_t cached_beacon_ts;
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
// Open PCAP for target on first EAPOL and flush cached beacon if any
// ---------------------------------------------------------------------------
static void hsc_ensure_pcap(HscTarget* t) {
    if(t->pcap_file || !s_hsc_storage) return;

    char path[128];
    hs_make_pcap_path(s_hsc_storage, t->ssid, t->bssid, path, sizeof(path));
    t->pcap_file = wifi_pcap_open(s_hsc_storage, path);
    if(!t->pcap_file) return;

    // Flush cached beacon — aircrack-ng needs it for the SSID/BSSID binding.
    if(t->cached_beacon_len > 0) {
        wifi_pcap_write_packet(
            t->pcap_file, t->cached_beacon_ts,
            t->cached_beacon, t->cached_beacon_len);
    }
}

// ---------------------------------------------------------------------------
// Process a single packet for multi-target capture
// ---------------------------------------------------------------------------
static void hsc_process_packet(const uint8_t* payload, int len, uint32_t ts_us) {
    if(len < 24) return;

    // Beacon — extract BSSID and SSID
    if(hs_is_beacon(payload, len)) {
        const uint8_t* bssid = &payload[16];
        HscTarget* t = hsc_find_or_create_target(bssid);
        if(!t) return;

        t->has_beacon = true;

        // Update SSID if not yet resolved (first beacon may be hidden,
        // a later one may carry the real SSID tag).
        if(!t->ssid_resolved) {
            char ssid_buf[33] = {0};
            if(hs_extract_beacon_ssid(payload, len, ssid_buf, sizeof(ssid_buf))) {
                strncpy(t->ssid, ssid_buf, sizeof(t->ssid) - 1);
                t->ssid[sizeof(t->ssid) - 1] = '\0';
                t->ssid_resolved = true;
            }
        }

        // Either cache the beacon (until PCAP exists) or write it directly.
        if(t->pcap_file) {
            wifi_pcap_write_packet(t->pcap_file, ts_us, payload, len);
        } else {
            uint16_t cache_len = (len > HSC_BEACON_CACHE_LEN) ? HSC_BEACON_CACHE_LEN : (uint16_t)len;
            memcpy(t->cached_beacon, payload, cache_len);
            t->cached_beacon_len = cache_len;
            t->cached_beacon_ts = ts_us;
        }
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
    UNUSED(station);
    UNUSED(ap);
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

    // Open PCAP on first EAPOL (flushes cached beacon) and write the EAPOL.
    hsc_ensure_pcap(t);
    if(t->pcap_file) {
        wifi_pcap_write_packet(t->pcap_file, ts_us, payload, len);
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
    // Data frame — quick EAPOL pre-filter
    else if(frame_type == 2) {
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) return; // 4-address WDS — not handled
        int hdr_len = 24;
        if((frame_subtype & 0x08) == 0x08) hdr_len += 2; // QoS
        if(fc & 0x8000) hdr_len += 4;                    // HT Control (Order)
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
    int budget = HSC_DRAIN_MAX_PER_TICK;
    while(s_hsc_read_idx != s_hsc_write_idx && budget-- > 0) {
        HscPkt* pkt = &s_hsc_pkt_pool[s_hsc_read_idx];
        hsc_process_packet(pkt->data, pkt->len, pkt->timestamp_us);
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

    // Clamp selection to current target count
    if(model->count == 0) {
        model->selected = 0;
        model->window_offset = 0;
    } else if(model->selected >= model->count) {
        model->selected = model->count - 1;
    }

    // Adjust window so selection is always visible
    if(model->selected < model->window_offset) {
        model->window_offset = model->selected;
    } else if(model->selected >= model->window_offset + HS_CHANNEL_ITEMS_ON_SCREEN) {
        model->window_offset = model->selected - HS_CHANNEL_ITEMS_ON_SCREEN + 1;
    }

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
// Channel switch helper
// ---------------------------------------------------------------------------
static void hsc_change_channel(uint8_t new_channel) {
    s_hsc_channel = new_channel;
    wifi_hal_set_promiscuous(false, NULL);
    wifi_hal_set_channel(s_hsc_channel);
    furi_delay_ms(20);
    wifi_hal_set_promiscuous(true, hsc_rx_callback);
    ESP_LOGI(TAG, "Channel changed to %d", s_hsc_channel);
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

    // Set channel and start promiscuous (single call — registers cb atomically)
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
            // Scroll target list up
            HsChannelViewModel* model = view_get_model(app->view_handshake_channel);
            if(model->selected > 0) model->selected--;
            view_commit_model(app->view_handshake_channel, true);
            consumed = true;
        } else if(event.event == InputKeyDown) {
            // Scroll target list down
            HsChannelViewModel* model = view_get_model(app->view_handshake_channel);
            if(model->count > 0 && model->selected + 1 < model->count) model->selected++;
            view_commit_model(app->view_handshake_channel, true);
            consumed = true;
        } else if(event.event == InputKeyRight) {
            uint8_t next = s_hsc_channel < 13 ? s_hsc_channel + 1 : 1;
            hsc_change_channel(next);
            consumed = true;
        } else if(event.event == InputKeyLeft) {
            uint8_t prev = s_hsc_channel > 1 ? s_hsc_channel - 1 : 13;
            hsc_change_channel(prev);
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

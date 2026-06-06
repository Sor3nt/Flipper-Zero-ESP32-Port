/* "Capture Handshake" feature (id 2) — passive WPA handshake capture.
 *
 * Capture logic ported from the T-Embed firmware's wlan_app channel-mode
 * (scene_handshake.c) + shared parser (buddy_hs_parser). The buddy has no SD, so
 * instead of writing a .pcap it keeps the captured EAPOL frames (M1..M4) + a
 * beacon PER BSSID in a DURABLE store (buddy_hs_store, RAM + NVS). Each complete
 * handshake (M2 & M3) is delivered to the master as one reliable, fragmented,
 * acked unit (BuddyWireResult); the master reassembles it into a .pcap. Because
 * the store is persistent and retain-until-ack, a captured handshake survives
 * arbitrary master absence AND a buddy reboot/power loss.
 *
 * AUTONOM: der Buddy capturet eigenständig weiter, egal ob der Master da ist.
 * Gestoppt wird NUR durch ein explizites FeatureStop oder Disconnect — KEIN
 * Watchdog. Promiscuous (Monitor) + ESP-NOW laufen parallel auf demselben
 * (Capture-)Kanal, also kann er gleichzeitig capturen, liefern und Kommandos
 * empfangen. Der Master sweept die Kanäle, findet den Buddy und tunt sich hin.
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
#include "buddy_hs_store.h"
#include "buddy_node.h"
#include "buddy_protocol.h"

#define FEAT_ID 2
#define TAG     "feat-hs"

#define CAP_CHANNEL_MIN 1
#define CAP_CHANNEL_MAX 13

#define CAP_PKT_MAX 512
#define CAP_POOL    48 /* rx→task SPSC ring; Frames werden binnen CAP_FLUSH_MS geparst */

#define CAP_FLUSH_MS 400 /* Intervall: Ring parsen+speichern → Status + ggf. liefern */

/* Master gilt als erreichbar, wenn innerhalb dieser Zeit ein Send quittiert
 * wurde. Nur dann werden gespeicherte Handshakes geliefert; sonst behalten. */
#define CAP_MASTER_PRESENT_MS 6000

/* Captured-frame ring (SPSC: promiscuous cb writes, capture task reads). */
typedef struct {
    uint16_t len;
    uint8_t data[CAP_PKT_MAX];
} CapPkt;

static CapPkt* s_pool = NULL;
static volatile uint32_t s_wr = 0;
static volatile uint32_t s_rd = 0;

static uint32_t s_eapol_count; /* nur für die Live-Status-Anzeige */

static volatile bool s_run;
static TaskHandle_t s_task;
static uint8_t s_channel; /* 0 = hop 1..13, else fixed */
static uint8_t s_master[6];

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
        /* beacon — für SSID/Kontext. Bei halbvollem Ring Beacons verwerfen, damit
         * Platz für EAPOL (die eigentlichen HS) bleibt. */
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

/* ─────── parse + durable store (capture task) ─────── */
static void note_frame(const uint8_t* payload, int len) {
    if(wlan_hs_is_beacon(payload, len)) {
        char ssid[33];
        wlan_hs_extract_beacon_ssid(payload, len, ssid, sizeof(ssid));
        buddy_hs_store_beacon(&payload[16], ssid, payload, (uint16_t)len);
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
    /* Frame durabel ablegen (SSID kommt aus dem Beacon). Bei Vollständigkeit (M2&M3)
     * wird der Record persistiert und beim nächsten Pump zuverlässig geliefert. */
    buddy_hs_store_eapol(bssid, "", msg, payload, (uint16_t)len);
}

static void send_summary(uint8_t cur_channel, uint8_t state) {
    char msg[32];
    int n = snprintf(
        msg,
        sizeof(msg),
        "ch%u E%lu HS%u",
        cur_channel,
        (unsigned long)s_eapol_count,
        buddy_hs_store_complete_count());
    if(n < 0) n = 0;
    buddy_node_feature_status(FEAT_ID, state, (const uint8_t*)msg, (uint8_t)n);
}

/* Ring parsen + speichern (IMMER, auch ohne Master), Status senden und — wenn der
 * Master erreichbar ist — gespeicherte Handshakes liefern. */
static void capture_tick(uint8_t cur_channel) {
    while(s_rd != s_wr) {
        CapPkt* p = &s_pool[s_rd];
        note_frame(p->data, p->len);
        s_rd = (s_rd + 1) % CAP_POOL;
    }
    send_summary(cur_channel, BuddyFeatStateData); /* Probe + Live-Status */
    if(buddy_espnow_ms_since_send_ok() < CAP_MASTER_PRESENT_MS && buddy_hs_store_has_pending()) {
        buddy_hs_store_pump(s_master);
    }
}

/* ─────── capture task ─────── */
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
        capture_tick(ch);
    }

    /* teardown: promiscuous off BEFORE the pool is freed (rx_cb race) */
    esp_wifi_set_promiscuous(false);
    capture_tick(ch); /* letztes Parsen + Liefern */
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
        ESP_LOGW(TAG, "no master — cannot deliver");
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
    s_eapol_count = 0;

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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

/** Starte den WiFi-Stack im STA-Mode. Stoppt zuvor ggf. den BT-Stack
 *  (geteiltes Radio). Idempotent — gibt true zurück, wenn der Stack bereits
 *  läuft oder erfolgreich initialisiert wurde. */
bool wlan_hal_start(void);

/** Beende den WiFi-Stack und stelle BT wieder her. Idempotent. */
void wlan_hal_stop(void);

bool wlan_hal_is_started(void);

/** Synchroner aktiver Scan auf allen Channels. Allokiert *out_records via
 *  malloc; Caller frees. Bei *out_count == 0 ist *out_records NULL. */
void wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

/** Versuche Verbindung zu SSID. Nicht-blockierend — polle wlan_hal_is_connected()
 *  oder warte auf den IP_EVENT. Auto-Reconnect ist aktiv bis wlan_hal_disconnect(). */
bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel);

void wlan_hal_disconnect(void);

bool wlan_hal_is_connected(void);

/** Eigene IP nach erfolgreichem Connect (Network-Byte-Order). 0 wenn keine. */
uint32_t wlan_hal_get_own_ip(void);

/** Netmask der STA-Verbindung (Network-Byte-Order). 0 wenn nicht verbunden. */
uint32_t wlan_hal_get_netmask(void);

/** Eigene MAC-Adresse der STA-Schnittstelle. Liefert false wenn WiFi nicht
 *  läuft. out muss 6 Bytes haben. */
bool wlan_hal_get_own_mac(uint8_t out[6]);

/** Gateway-IP (Network-Byte-Order). 0 wenn nicht verbunden. */
uint32_t wlan_hal_get_gw_ip(void);

/** Sende einen rohen Ethernet-Frame (Eth-Header + Payload) über die STA-IF
 *  via esp_wifi_internal_tx(). Für ARP-Injection durch wlan_netcut. */
bool wlan_hal_send_eth_raw(const uint8_t* data, uint16_t len);

/** Setzt den primären Kanal (1..14). */
void wlan_hal_set_channel(uint8_t channel);

/** Aktiviert/deaktiviert Promiscuous-RX. cb wird beim Aktivieren gesetzt
 *  und kann NULL sein (z.B. zum bloßen Channel-Setzen). */
void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb);

/** Sende einen rohen 802.11-Frame (für Deauth/Disassoc) via
 *  esp_wifi_80211_tx(). Frame-Länge bis 64 Bytes. */
bool wlan_hal_send_raw(const uint8_t* data, uint16_t len);

/** Generischer Worker-Hook: ruft fn(arg) im wlan-Worker-Task auf und blockt
 *  bis er fertig ist. Nötig für Code, der esp_wifi_*-APIs vom Worker aus
 *  treiben muss (z.B. Evil Portal: SoftAP-Init, Verify-Connect). */
typedef void (*WlanHalWorkerFn)(void* arg);
bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg);

/** Beacon-Spam-Modi für wlan_hal_beacon_spam_start(). */
typedef enum {
    WlanHalBeaconModeFunny,
    WlanHalBeaconModeRickroll,
    WlanHalBeaconModeRandom,
    WlanHalBeaconModeCustom,
} WlanHalBeaconMode;

/** Startet einen Hintergrund-Task, der Beacon-Frames mit zufälligen MACs
 *  und SSIDs aus der gewählten Quelle sendet. Channel rotiert alle 5
 *  Pakete (1..11). base_ssid wird nur im Custom-Mode verwendet (Suffix
 *  Counter). Kein Effekt wenn bereits running. */
void wlan_hal_beacon_spam_start(WlanHalBeaconMode mode, const char* base_ssid);

/** Stoppt den Beacon-Task synchron. Idempotent. */
void wlan_hal_beacon_spam_stop(void);

bool wlan_hal_beacon_spam_is_running(void);

uint32_t wlan_hal_beacon_spam_get_frame_count(void);

// ---------------------------------------------------------------------------
// Evil Portal: SoftAP + DNS-Hijack + HTTP-Captive-Server.
// ---------------------------------------------------------------------------

typedef void (*WlanHalEvilPortalCredCb)(const char* user, const char* pwd, void* ctx);
typedef void (*WlanHalEvilPortalValidCb)(const char* ssid, const char* pwd, void* ctx);
typedef void (*WlanHalEvilPortalBusyCb)(bool busy, const char* msg, void* ctx);

typedef struct {
    const char* ssid;
    uint8_t channel;
    bool verify_creds;            // Router-Mode: gegen echte APs verifizieren
    const char* html;
    size_t html_len;
    const char* router_ssid_options; // optional, ersetzt %SSID_OPTIONS%
    WlanHalEvilPortalCredCb cred_cb;
    void* cred_cb_ctx;
    WlanHalEvilPortalValidCb valid_cb;
    void* valid_cb_ctx;
    WlanHalEvilPortalBusyCb busy_cb;
    void* busy_cb_ctx;
} WlanHalEvilPortalConfig;

bool wlan_hal_evil_portal_start(const WlanHalEvilPortalConfig* cfg);
void wlan_hal_evil_portal_stop(void);
bool wlan_hal_evil_portal_is_running(void);
bool wlan_hal_evil_portal_verify_creds(const char* ssid, const char* pwd);
void wlan_hal_evil_portal_pause(void);
void wlan_hal_evil_portal_resume(void);
bool wlan_hal_evil_portal_is_paused(void);
uint32_t wlan_hal_evil_portal_get_cred_count(void);
uint16_t wlan_hal_evil_portal_get_client_count(void);

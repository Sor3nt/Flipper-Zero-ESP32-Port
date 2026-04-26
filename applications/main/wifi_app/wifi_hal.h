#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

void wifi_hal_preinit(void);

typedef void (*WifiHalWorkerFn)(void* arg);

bool wifi_hal_run_in_worker(WifiHalWorkerFn fn, void* arg);

bool wifi_hal_start(void);

void wifi_hal_stop(void);

void wifi_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

void wifi_hal_set_channel(uint8_t channel);

void wifi_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb);

bool wifi_hal_send_raw(const uint8_t* data, uint16_t len);

bool wifi_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel);

void wifi_hal_disconnect(void);

bool wifi_hal_is_connected(void);

bool wifi_hal_is_started(void);

void wifi_hal_cleanup(void);

void wifi_hal_cleanup_keep_connection(void);

typedef enum {
    WifiHalBeaconModeFunny,
    WifiHalBeaconModeRickroll,
    WifiHalBeaconModeRandom,
    WifiHalBeaconModeCustom,
} WifiHalBeaconMode;

void wifi_hal_beacon_spam_start(WifiHalBeaconMode mode, const char* base_ssid);

void wifi_hal_beacon_spam_stop(void);

bool wifi_hal_beacon_spam_is_running(void);

uint32_t wifi_hal_beacon_spam_get_frame_count(void);

typedef void (*WifiHalEvilPortalCredCb)(const char* user, const char* pwd, void* ctx);
typedef void (*WifiHalEvilPortalValidCb)(const char* ssid, const char* pwd, void* ctx);
typedef void (*WifiHalEvilPortalBusyCb)(bool busy, const char* msg, void* ctx);

typedef struct {
    const char* ssid;
    uint8_t channel;
    bool verify_creds;
    const char* html;
    size_t html_len;
    /** Optional pre-built <option>...</option> list substituted into
     *  %SSID_OPTIONS% inside the router HTML. NULL = no SSID dropdown. */
    const char* router_ssid_options;
    WifiHalEvilPortalCredCb cred_cb;
    void* cred_cb_ctx;
    WifiHalEvilPortalValidCb valid_cb;
    void* valid_cb_ctx;
    WifiHalEvilPortalBusyCb busy_cb;
    void* busy_cb_ctx;
} WifiHalEvilPortalConfig;

bool wifi_hal_evil_portal_start(const WifiHalEvilPortalConfig* cfg);

void wifi_hal_evil_portal_stop(void);

bool wifi_hal_evil_portal_is_running(void);

/** Try to associate to a real AP using the provided creds. Blocks ~6s.
 *  Only works when the portal was started with cfg.verify_creds = true. */
bool wifi_hal_evil_portal_verify_creds(const char* ssid, const char* pwd);

/** Suspend the AP (clients lose connectivity, HTTP/DNS continue to be set up)
 *  without tearing down the whole stack. Idempotent. */
void wifi_hal_evil_portal_pause(void);

/** Resume after pause. No-op if not paused. */
void wifi_hal_evil_portal_resume(void);

bool wifi_hal_evil_portal_is_paused(void);

uint32_t wifi_hal_evil_portal_get_cred_count(void);

uint16_t wifi_hal_evil_portal_get_client_count(void);
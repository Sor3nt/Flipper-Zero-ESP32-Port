#ifndef DLNA_WIFI_H
#define DLNA_WIFI_H

/* Slim STA-only WiFi connect for the video player DLNA output.
 *
 * This is a deliberately trimmed re-implementation of the wlan_app's wlan_hal
 * (the MP3 player is a separate FAP and cannot reach wlan_hal's internals). It
 * keeps only what the DLNA sender needs: bring up esp_wifi in STA mode,
 * scan, connect, and hand out the STA IP. All esp_wifi / lwIP work runs on a
 * dedicated FreeRTOS worker task (never a FuriThread — lwIP sockets and
 * esp_wifi_* must be driven from an xTaskCreate task).
 *
 * Like wlan_hal it stops the BT stack on start (shared radio + internal RAM)
 * and restores it on stop.
 */

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

/* Bring up the WiFi stack in STA mode (stops BT first). Idempotent. */
bool dlna_wifi_start(void);

/* Tear the WiFi stack down and restore BT. Idempotent. */
void dlna_wifi_stop(void);

bool dlna_wifi_is_started(void);

/* Synchronous active scan on all channels (runs on the worker, blocks the
 * caller a few seconds). Allocates *out_records via malloc; caller frees.
 * *out_records is NULL when *out_count == 0. */
void dlna_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

/* Start connecting to ssid. Non-blocking — poll dlna_wifi_is_connected().
 * Auto-reconnect stays active until dlna_wifi_disconnect(). bssid/channel
 * may be NULL/0. */
bool dlna_wifi_connect(
    const char* ssid,
    const char* password,
    const uint8_t* bssid,
    uint8_t channel);

void dlna_wifi_disconnect(void);

bool dlna_wifi_is_connected(void);

/* True if the last disconnect looked like an auth failure (wrong password). */
bool dlna_wifi_last_fail_is_auth(void);

/* Own STA IP in network byte order, 0 if not connected. */
uint32_t dlna_wifi_get_own_ip(void);

/* Run fn(arg) on the WiFi worker task and block until it returns. Required for
 * any code that must issue lwIP socket calls (the mDNS resolver, the RAOP
 * streamer). Returns false if the worker isn't up. */
typedef void (*DlnaWifiWorkerFn)(void* arg);
bool dlna_wifi_run_in_worker(DlnaWifiWorkerFn fn, void* arg);

#endif /* DLNA_WIFI_H */

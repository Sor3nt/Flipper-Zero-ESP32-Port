#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

/** No-op (kept for API compat). */
void wifi_hal_preinit(void);

/** Start WiFi (STA mode). Stops BLE if needed. All WiFi API calls
 *  are routed through a persistent worker task with internal RAM stack.
 *  @return true on success */
bool wifi_hal_start(void);

/** Stop WiFi, destroy worker, restart BLE if it was stopped. */
void wifi_hal_stop(void);

/** Perform a blocking WiFi scan. Caller must free *out_records. */
void wifi_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

/** Set WiFi channel (1-14). */
void wifi_hal_set_channel(uint8_t channel);

/** Enable/disable promiscuous mode with optional callback. */
void wifi_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb);

/** Send raw 802.11 frame (fire-and-forget via worker queue).
 *  Data is copied into the queue command (max 64 bytes).
 *  @return true if queued successfully, false if queue full or len > 64 */
bool wifi_hal_send_raw(const uint8_t* data, uint16_t len);

/** Connect to an AP. SSID and password are copied internally.
 *  Connection is async — poll wifi_hal_is_connected() for result.
 *  @param ssid     null-terminated SSID
 *  @param password null-terminated password (NULL or "" for open networks)
 *  @param bssid    6-byte BSSID to target specific AP (NULL for any)
 *  @param channel  channel hint (0 for auto)
 *  @return true if connect command was sent successfully */
bool wifi_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel);

/** Disconnect from current AP. */
void wifi_hal_disconnect(void);

/** Check if WiFi STA is connected to an AP (has IP). */
bool wifi_hal_is_connected(void);

/** Check if WiFi is currently started. */
bool wifi_hal_is_started(void);

/** Destroy the persistent worker task (call on app exit). */
void wifi_hal_cleanup(void);

/** Cleanup but keep WiFi connected (for app exit while staying online). */
void wifi_hal_cleanup_keep_connection(void);

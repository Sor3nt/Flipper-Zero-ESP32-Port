#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

/* Minimal WiFi scan helper for the NRF24 app — does not depend on the wifi_app.
 * Caller owns the returned records buffer (must free()). */
bool nrf24_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

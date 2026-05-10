#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct WlanApp WlanApp;

/** Startet Promiscuous-RX und sammelt Station-MACs in einen internen
 *  Lock-free Ring-Buffer. Aufrufer muss anschließend periodisch
 *  wlan_client_scanner_drain(app) aus dem Scene-Tick aufrufen.
 *
 *  bssid_filter: wenn != NULL, werden nur Frames akzeptiert, deren BSSID
 *                exakt matcht (SSID-Mode). NULL = alle Stations auf dem
 *                Channel (Channel-Mode).
 *  channel:      1..13. 0 = unverändert lassen. */
void wlan_client_scanner_start(const uint8_t* bssid_filter, uint8_t channel);

/** Schaltet Promiscuous ab und leert den Ring-Buffer. */
void wlan_client_scanner_stop(void);

/** Liest neue Events aus dem Ring-Buffer und merged sie in
 *  app->deauth_clients (max WLAN_APP_MAX_DEAUTH_CLIENTS). RSSI wird
 *  gewichtet geglättet. Returns true, wenn sich Liste oder RSSI geändert
 *  haben (Caller kann dann das View neu zeichnen). */
bool wlan_client_scanner_drain(WlanApp* app);

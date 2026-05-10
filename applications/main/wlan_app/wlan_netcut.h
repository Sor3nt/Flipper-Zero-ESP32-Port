#pragma once

#include "wlan_app.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct WlanNetcut WlanNetcut;

WlanNetcut* wlan_netcut_alloc(void);
void wlan_netcut_free(WlanNetcut* nc);

/** my_ip / my_mac / gw_ip / netmask sammeln und gw_mac aus der ARP-Tabelle lesen.
 *  Liefert false wenn STA nicht verbunden ist oder gw_mac nicht ermittelbar. */
bool wlan_netcut_preflight(WlanNetcut* nc);

/** Synchronisiert die App-Device-Liste in den Netcut-Zustand:
 *  - device.block_internet=true  → Cut
 *  - device.throttle_kbps>0      → Throttle
 *  - sonst                       → Idle (Restore-Frames für 5 s)
 *  Startet einen Worker und installiert den L2-Hook beim ersten aktiven Device,
 *  stoppt + entfernt L2-Hook wenn alle wieder Idle.
 *  @return true wenn mindestens ein Device gerade von Cut/Throttle → Idle
 *          übergeht (5-Sekunden-Restore-Window aktiv) — caller kann den
 *          User mit einem "Restoring device..."-Popup informieren. */
bool wlan_netcut_apply(WlanNetcut* nc, const WlanDeviceRecord* devices, uint8_t count);

/** Sofort alle aktiven Devices restore + Worker beenden + L2-Hook entfernen. */
void wlan_netcut_stop(WlanNetcut* nc);

bool wlan_netcut_is_running(WlanNetcut* nc);

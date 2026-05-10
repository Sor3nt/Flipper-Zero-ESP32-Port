#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct WlanOuiTable WlanOuiTable;

/** Lädt die OUI→Vendor-Tabelle von SD (`/ext/apps_data/wifi/mac-vendor.txt`).
 *  Format pro Zeile: `XX:XX:XX,Vendor Name` (Komma-Trenner). Zeilen, die mit
 *  '#' beginnen oder nicht parsbar sind, werden ignoriert. Returns NULL,
 *  wenn die Datei fehlt oder keine gültigen Einträge enthält — Caller
 *  behandelt dann jeden Lookup als Miss. */
WlanOuiTable* wlan_oui_load(void);

void wlan_oui_free(WlanOuiTable* t);

/** Lookup auf die ersten 3 Bytes der MAC. Returns NULL bei Miss oder
 *  wenn t == NULL. Pointer ist gültig bis wlan_oui_free(). */
const char* wlan_oui_lookup(WlanOuiTable* t, const uint8_t mac[6]);

#pragma once

#include <gui/view.h>
#include "../wlan_cred_sniff.h"

typedef struct WlanLiveCredsView WlanLiveCredsView;

WlanLiveCredsView* wlan_live_creds_view_alloc(void);
void wlan_live_creds_view_free(WlanLiveCredsView* v);
View* wlan_live_creds_view_get_view(WlanLiveCredsView* v);

/** Aktuellen Snapshot (neueste zuerst) übergeben. Aus dem GUI/Tick-Thread
 *  aufrufen. */
void wlan_live_creds_view_set_entries(
    WlanLiveCredsView* v, const WlanCredEntry* arr, uint32_t count);

/** Anzahl monitored Opfer-Devices setzen (Header zeigt "N victims"). */
void wlan_live_creds_view_set_victim_count(WlanLiveCredsView* v, uint16_t count);

/** View zurück in den Listen-Modus zwingen (Scene on_enter). */
void wlan_live_creds_view_reset(WlanLiveCredsView* v);

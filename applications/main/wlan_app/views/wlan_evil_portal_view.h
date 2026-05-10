#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct WlanEvilPortalView WlanEvilPortalView;

typedef void (*WlanEvilPortalViewActionCb)(void* ctx);

WlanEvilPortalView* wlan_evil_portal_view_alloc(void);
void wlan_evil_portal_view_free(WlanEvilPortalView* v);
View* wlan_evil_portal_view_get_view(WlanEvilPortalView* v);

void wlan_evil_portal_view_set_ssid(WlanEvilPortalView* v, const char* ssid);
void wlan_evil_portal_view_set_channel(WlanEvilPortalView* v, uint8_t channel);
void wlan_evil_portal_view_set_clients(WlanEvilPortalView* v, uint16_t count);
void wlan_evil_portal_view_set_creds(WlanEvilPortalView* v, uint32_t count);
void wlan_evil_portal_view_set_last(WlanEvilPortalView* v, const char* user);
void wlan_evil_portal_view_set_busy(WlanEvilPortalView* v, bool busy, const char* msg);
void wlan_evil_portal_view_set_paused(WlanEvilPortalView* v, bool paused);
void wlan_evil_portal_view_set_action_callback(
    WlanEvilPortalView* v, WlanEvilPortalViewActionCb cb, void* ctx);

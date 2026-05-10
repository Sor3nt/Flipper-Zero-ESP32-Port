#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct WlanEvilPortalCapturedView WlanEvilPortalCapturedView;

typedef void (*WlanEvilPortalCapturedBackCb)(void* ctx);

WlanEvilPortalCapturedView* wlan_evil_portal_captured_view_alloc(void);
void wlan_evil_portal_captured_view_free(WlanEvilPortalCapturedView* v);
View* wlan_evil_portal_captured_view_get_view(WlanEvilPortalCapturedView* v);

void wlan_evil_portal_captured_view_set_data(
    WlanEvilPortalCapturedView* v, const char* ssid, const char* pwd);

void wlan_evil_portal_captured_view_set_back_callback(
    WlanEvilPortalCapturedView* v, WlanEvilPortalCapturedBackCb cb, void* ctx);

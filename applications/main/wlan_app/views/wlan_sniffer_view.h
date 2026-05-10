#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct WlanSnifferView WlanSnifferView;

typedef enum {
    WlanSnifferViewActionToggle,
    WlanSnifferViewActionTargets,
    WlanSnifferViewActionChannelUp,
    WlanSnifferViewActionChannelDown,
} WlanSnifferViewAction;

typedef void (*WlanSnifferViewActionCb)(WlanSnifferViewAction action, void* ctx);

WlanSnifferView* wlan_sniffer_view_alloc(void);
void wlan_sniffer_view_free(WlanSnifferView* v);
View* wlan_sniffer_view_get_view(WlanSnifferView* v);

void wlan_sniffer_view_set_title(WlanSnifferView* v, const char* title);
void wlan_sniffer_view_set_target_count(WlanSnifferView* v, uint8_t count);
void wlan_sniffer_view_set_channel_mode(WlanSnifferView* v, bool channel_mode);
void wlan_sniffer_view_set_channel(WlanSnifferView* v, uint8_t channel);
void wlan_sniffer_view_set_received(WlanSnifferView* v, uint32_t count);
void wlan_sniffer_view_set_elapsed(WlanSnifferView* v, uint32_t sec);
void wlan_sniffer_view_set_running(WlanSnifferView* v, bool running);
void wlan_sniffer_view_reset_counters(WlanSnifferView* v);
void wlan_sniffer_view_set_action_callback(
    WlanSnifferView* v, WlanSnifferViewActionCb cb, void* ctx);

#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>

#define WLAN_CONNECT_VIEW_MAX_APS 64
#define WLAN_CONNECT_VIEW_SSID_MAX 33
#define WLAN_CONNECT_VIEW_MAX_MENU_ITEMS 6
#define WLAN_CONNECT_VIEW_MENU_LABEL_MAX 20

typedef struct {
    char ssid[WLAN_CONNECT_VIEW_SSID_MAX];
    bool unlocked; // true = offen oder Passwort bekannt
    uint16_t user_id;
} WlanConnectViewAp;

typedef struct {
    char label[WLAN_CONNECT_VIEW_MENU_LABEL_MAX];
    uint16_t user_id;
} WlanConnectMenuItem;

typedef struct {
    WlanConnectViewAp aps[WLAN_CONNECT_VIEW_MAX_APS];
    uint8_t ap_count;
    uint8_t selected;
    uint8_t window_offset;

    bool menu_open;
    WlanConnectMenuItem menu_items[WLAN_CONNECT_VIEW_MAX_MENU_ITEMS];
    uint8_t menu_count;
    uint8_t menu_selected;
} WlanConnectViewModel;

View* wlan_connect_view_alloc(void);
void wlan_connect_view_free(View* view);
void wlan_connect_view_set_view_dispatcher(View* view, ViewDispatcher* vd);

void wlan_connect_view_clear(View* view);
void wlan_connect_view_add_ap(View* view, const char* ssid, bool unlocked, uint16_t user_id);

void wlan_connect_view_set_selected(View* view, uint8_t index);
uint8_t wlan_connect_view_get_selected(View* view);

/* ----- Modal-Submenu (Long-OK auf SSID) ----- */
void wlan_connect_view_clear_menu(View* view);
void wlan_connect_view_add_menu_item(View* view, const char* label, uint16_t user_id);
void wlan_connect_view_open_menu(View* view);
void wlan_connect_view_close_menu(View* view);
bool wlan_connect_view_is_menu_open(View* view);
uint8_t wlan_connect_view_get_menu_selected(View* view);
WlanConnectMenuItem wlan_connect_view_get_menu_item(View* view, uint8_t index);

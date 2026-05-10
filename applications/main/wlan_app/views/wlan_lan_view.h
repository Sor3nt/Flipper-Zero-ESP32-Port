#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/icon.h>

#define WLAN_LAN_VIEW_LABEL_MAX 28
#define WLAN_LAN_VIEW_MAX_ITEMS 80
#define WLAN_LAN_VIEW_MAX_MENU_ITEMS 6
#define WLAN_LAN_VIEW_MENU_LABEL_MAX 20

typedef enum {
    WlanLanItemKindAction = 0,
    WlanLanItemKindDevice = 1,
    WlanLanItemKindSeparator = 2,
} WlanLanItemKind;

#define WLAN_LAN_VIEW_VALUE_MAX 12

typedef struct {
    WlanLanItemKind kind;
    char label[WLAN_LAN_VIEW_LABEL_MAX];
    char label2[WLAN_LAN_VIEW_LABEL_MAX]; // optional, 2. Spalte (z.B. Hostname)
    char value_label[WLAN_LAN_VIEW_VALUE_MAX]; // Text rechts (z.B. "VIP", "CUT")
    const Icon* value_icon;                    // hat Vorrang vor value_label
    bool is_default; // false → in Header-Counter <sel/total> als ausgewählt zählen
    bool centered;   // Label horizontal zentriert
    bool col1_small; // col1 in FontBatteryPercent rendern (z.B. "99%" RSSI)
    uint16_t user_id;
} WlanLanItem;

typedef struct {
    char label[WLAN_LAN_VIEW_MENU_LABEL_MAX];
    uint16_t user_id;
} WlanLanMenuItem;

typedef struct {
    char header_title[16]; // Default "LAN"
    char back_label[16];    // Wenn != "" → Soft-Button "< <label>" am unteren Rand
    char empty_text[32];    // Wenn item_count==0 und gesetzt: framed Text mittig
    bool counter_show_selection_always; // true → Counter immer als <sel>/<total>, auch bei sel=0
    WlanLanItem items[WLAN_LAN_VIEW_MAX_ITEMS];
    uint8_t item_count;
    uint8_t selected;
    uint8_t window_offset;

    bool menu_open;
    WlanLanMenuItem menu_items[WLAN_LAN_VIEW_MAX_MENU_ITEMS];
    uint8_t menu_count;
    uint8_t menu_selected;
} WlanLanViewModel;

View* wlan_lan_view_alloc(void);
void wlan_lan_view_free(View* view);

/** ViewDispatcher für custom events (OK fires WlanAppCustomEventLanItemOk). */
void wlan_lan_view_set_view_dispatcher(View* view, ViewDispatcher* vd);

/** Setzt den Header-Titel (zentriert; Default "LAN"). */
void wlan_lan_view_set_header_title(View* view, const char* title);

/** Setzt einen optionalen Back-Soft-Button am unteren Rand.
 *  Hardware-Back im Scene-Manager bleibt für die Action zuständig. NULL/leer
 *  = kein Button (volle Listenhöhe). */
void wlan_lan_view_set_back_label(View* view, const char* label);

/** Wenn die Liste leer ist (item_count==0) und ein Text gesetzt ist, wird
 *  dieser zentriert in einem gerundeten Rahmen statt "(empty)" gezeichnet.
 *  NULL/leer = Default ("(empty)" Text ohne Rahmen). */
void wlan_lan_view_set_empty_text(View* view, const char* text);

/** Wenn true: Counter rechts oben wird immer als "<sel>/<total>" gerendert,
 *  auch wenn sel=0. Default false → bei sel=0 wird nur "<total>" gezeigt. */
void wlan_lan_view_set_force_selection_counter(View* view, bool force);

void wlan_lan_view_clear(View* view);

/** kind=Action, links-bündig. */
void wlan_lan_view_add_action(View* view, const char* label, uint16_t user_id);

/** kind=Action, horizontal zentriert. */
void wlan_lan_view_add_action_centered(View* view, const char* label, uint16_t user_id);

/** kind=Device. col1 (z.B. IP) wird links, col2 (z.B. Hostname) bündig
 *  ab der Pixel-Breite des längsten col1 + Spacer gerendert; rechts wird
 *  entweder das value_icon (falls != NULL) oder der value_label-Text
 *  gerendert. is_default=false markiert den Eintrag als "ausgewählt"
 *  fürs Header-Counter <sel>/<total>. */
void wlan_lan_view_add_device(
    View* view,
    const char* col1,
    const char* col2,
    const char* value_label,
    const Icon* value_icon,
    bool is_default,
    uint16_t user_id);

/** Wie add_device, aber col1 wird in kleinerer Schrift (FontBatteryPercent)
 *  gerendert — sinnvoll für kompakte Werte wie "99%" RSSI. */
void wlan_lan_view_add_device_compact(
    View* view,
    const char* col1,
    const char* col2,
    const char* value_label,
    const Icon* value_icon,
    bool is_default,
    uint16_t user_id);

/** Horizontale Trennlinie. Nicht selektierbar (Up/Down skipt). */
void wlan_lan_view_add_separator(View* view);

void wlan_lan_view_set_selected(View* view, uint8_t index);
uint8_t wlan_lan_view_get_selected(View* view);

/** Lokale Kopie. */
WlanLanItem wlan_lan_view_get_item(View* view, uint8_t index);

void wlan_lan_view_set_device_value(
    View* view,
    uint8_t index,
    const char* value_label,
    const Icon* value_icon,
    bool is_default);

/** Wie set_device_value, aber sucht das Device über seine user_id. */
void wlan_lan_view_set_device_value_by_user_id(
    View* view,
    uint16_t user_id,
    const char* value_label,
    const Icon* value_icon,
    bool is_default);

/* ----- Modal-Submenu (Long-OK auf Device) ----- */
void wlan_lan_view_clear_menu(View* view);
void wlan_lan_view_add_menu_item(View* view, const char* label, uint16_t user_id);
void wlan_lan_view_open_menu(View* view);
void wlan_lan_view_close_menu(View* view);
bool wlan_lan_view_is_menu_open(View* view);
uint8_t wlan_lan_view_get_menu_selected(View* view);
WlanLanMenuItem wlan_lan_view_get_menu_item(View* view, uint8_t index);

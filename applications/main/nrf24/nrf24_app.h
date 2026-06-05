#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <esp_wifi.h>

#include "views/nrf24_spectrum_view.h"
#include "views/nrf24_jam_view.h"
#include "views/nrf24_scan_view.h"
#include "views/nrf24_mj_scan_view.h"
#include "views/nrf24_mj_attack_view.h"
#include "helpers/nrf24_mj_core.h"
#include "helpers/nrf24_channel_source.h"
#include "scenes/scenes.h"

typedef enum {
    Nrf24ViewSubmenu,
    Nrf24ViewWidget,
    Nrf24ViewSpectrum,
    Nrf24ViewJam,
    Nrf24ViewScan,
    Nrf24ViewJamConfig,
    Nrf24ViewMjScan,
    Nrf24ViewMjAttack,
} Nrf24View;

#define NRF24_WIFI_SCAN_MAX 24

typedef struct Nrf24App {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    VariableItemList* var_item_list;
    DialogsApp* dialogs;
    Storage* storage;

    View* spectrum_view;
    View* jam_view; /* unified jam engine */
    View* scan_view; /* activity-scan progress */
    View* mj_scan_view;
    View* mj_attack_view;

    /* Unified jammer selection / scan-cache state */
    Nrf24JamState jam;

    /* WiFi scan results — owned by the wifi-scan scene, consumed by the
     * WiFi channel source. */
    wifi_ap_record_t* wifi_aps;
    uint16_t wifi_ap_count;
    char selected_wifi_ssid[33];
    uint8_t selected_wifi_channel;

    /* MouseJacker shared state (owned by the mj scenes) */
    MjTarget mj_targets[MJ_MAX_TARGETS];
    uint8_t mj_target_count;
    int8_t mj_selected_target; /* -1 = none */
    FuriString* mj_script_path;
    bool mj_auto_mode;
} Nrf24App;

#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <esp_wifi.h>

#include "views/nrf24_spectrum_view.h"
#include "views/nrf24_ch_jammer_view.h"
#include "views/nrf24_wifi_jam_view.h"
#include "views/nrf24_smart_jam_view.h"
#include "views/nrf24_mj_scan_view.h"
#include "views/nrf24_mj_attack_view.h"
#include "helpers/nrf24_mj_core.h"
#include "scenes/scenes.h"

typedef enum {
    Nrf24ViewSubmenu,
    Nrf24ViewWidget,
    Nrf24ViewSpectrum,
    Nrf24ViewChJammer,
    Nrf24ViewWifiJam,
    Nrf24ViewSmartJam,
    Nrf24ViewMjScan,
    Nrf24ViewMjAttack,
} Nrf24View;

#define NRF24_WIFI_SCAN_MAX 24

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    DialogsApp* dialogs;
    Storage* storage;

    View* spectrum_view;
    View* ch_jammer_view;
    View* wifi_jam_view;
    View* smart_jam_view;
    View* mj_scan_view;
    View* mj_attack_view;

    /* WiFi scan results — owned by the wifi_scan scene */
    wifi_ap_record_t* wifi_aps;
    uint16_t wifi_ap_count;

    /* Selected AP for the WiFi jammer scene */
    char selected_wifi_ssid[33];
    uint8_t selected_wifi_channel;

    /* MouseJacker shared state (owned by the mj scenes) */
    MjTarget mj_targets[MJ_MAX_TARGETS];
    uint8_t mj_target_count;
    int8_t mj_selected_target; /* -1 = none */
    FuriString* mj_script_path;
    bool mj_auto_mode;
} Nrf24App;

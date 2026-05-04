#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>

#include "ble_tracker_hal.h"
#include "scenes/scenes.h"

#define BLE_SPAM_LOG_TAG "BleSpam"

typedef enum {
    BleSpamAttackAppleDevice,
    BleSpamAttackAppleAction,
    BleSpamAttackAppleNotYourDevice,
    BleSpamAttackFastPair,
    BleSpamAttackSwiftPair,
    BleSpamAttackSamsungBuds,
    BleSpamAttackSamsungWatch,
    BleSpamAttackXiaomi,
    BleSpamAttackPairSpam,
    BleSpamAttackPairSpamRickroll,
    BleSpamAttackPairSpamCustom,
    BleSpamAttackCount,
} BleSpamAttackType;

typedef enum {
    BleSpamCustomEventToggle = 100,
    BleSpamCustomEventSpeedUp,
    BleSpamCustomEventSpeedDown,
} BleSpamCustomEvent;

typedef enum {
    BleSpamCustomEventWalkConnect = 200,
} BleSpamWalkEvent;

typedef enum {
    BleSpamViewSubmenu,
    BleSpamViewRunning,
    BleSpamViewTextInput,
    BleSpamViewWalkScan,
    BleSpamViewWalkDetail,
    BleSpamViewAutoWalk,
    BleSpamViewTrackerScan,
    BleSpamViewTrackerGeiger,
    BleSpamViewRaceDetector,
} BleSpamViewId;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    View* view_running;
    TextInput* text_input;

    // Attack state
    BleSpamAttackType attack_type;
    volatile bool running;
    uint32_t packet_count;
    uint32_t delay_ms;
    uint16_t current_index;
    char current_device[48];
    char custom_pair_name[32];

    // BLE Walk state
    View* view_walk_scan;
    View* view_walk_detail;
    uint16_t walk_selected_device;
    uint16_t walk_selected_service;
    uint16_t walk_selected_char;

    // BLE Auto-Walk state
    View* view_auto_walk;

    // BLE Tracker state
    View* view_tracker_scan;
    View* view_tracker_geiger;
    TrackerDevice tracker_target;
    FuriTimer* tracker_geiger_timer;
    volatile int8_t tracker_current_rssi;
    volatile bool tracker_current_stale;
    uint32_t tracker_current_period;

    // Airoha RACE Detector state (CVE-2025-20700)
    View* view_race_detector;
    volatile bool race_probe_abort;
} BleSpamApp;

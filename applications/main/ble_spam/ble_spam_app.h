#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>

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
    BleSpamViewWalkScan,
    BleSpamViewWalkDetail,
} BleSpamViewId;

typedef struct {
    Gui* gui;
    SceneManager* scene_manager;
    ViewDispatcher* view_dispatcher;

    Submenu* submenu;
    View* view_running;

    // Attack state
    BleSpamAttackType attack_type;
    volatile bool running;
    uint32_t packet_count;
    uint32_t delay_ms;
    uint16_t current_index;
    char current_device[48];

    // BLE Walk state
    View* view_walk_scan;
    View* view_walk_detail;
    uint16_t walk_selected_device;
    uint16_t walk_selected_service;
    uint16_t walk_selected_char;

    // BLE Clone state
    uint16_t clone_selected_device;
    volatile bool clone_active;
} BleSpamApp;

#pragma once

#include "scenes/clock_settings_scene.h"

#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include "views/clock_settings_module.h"

typedef struct ClockSettings ClockSettings;

struct ClockSettings {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    ClockSettingsModule* pwm_view;
};

typedef enum {
    ClockSettingsViewPwm,
} ClockSettingsView;

typedef enum {
    ClockSettingsCustomEventNone,
} ClockSettingsCustomEvent;

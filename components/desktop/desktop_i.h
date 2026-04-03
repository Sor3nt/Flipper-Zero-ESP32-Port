#pragma once

#include "desktop.h"
#include "desktop_settings.h"
#include "animations/animation_manager.h"
#include "views/desktop_view_main.h"

#include <gui.h>
#include <view_stack.h>
#include <view_dispatcher.h>
#include <scene_manager.h>

#include <loader.h>

typedef enum {
    DesktopViewIdMain,
    DesktopViewIdTotal,
} DesktopViewId;

struct Desktop {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    DesktopMainView* main_view;
    ViewStack* main_view_stack;

    Loader* loader;

    AnimationManager* animation_manager;
    FuriSemaphore* animation_semaphore;

    DesktopSettings settings;
    bool in_transition;
    bool app_running;
};

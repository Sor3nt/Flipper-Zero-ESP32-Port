#pragma once

#include <gui/gui.h>
#include <gui/view.h>
#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <ble_hid/ble_hid.h>

#include "scenes/bad_ble_scene.h"

#define BAD_BLE_APP_PATH_FOLDER "/ext/badble"
#define BAD_BLE_APP_EXTENSION ".ducky"

typedef enum {
    BadBleViewSubmenu,
    BadBleViewSettings,
    BadBleViewTextInput,
    BadBleViewPopup,
} BadBleView;

typedef enum {
    BadBleSceneStart,
    BadBleSceneSettings,
    BadBleSceneRun,
    BadBleSceneList,
    BadBleSceneCount,
} BadBleScene;

typedef struct {
    char device_name[32];
} BadBleSettings;

struct BadBleApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Storage* storage;
    
    Submenu* submenu;
    PopUp* popup;
    VariableItemList* var_item_list;
    TextInput* text_input;
    
    BleHid* ble_hid;
    BadBleSettings settings;
    
    FuriString* file_path;
    FuriString* script_path;
    char input_buffer[64];
    
    bool is_running;
    bool connected;
};

void bad_ble_scene_start_on_enter(void* context);
bool bad_ble_scene_start_on_event(void* context, SceneManagerEvent event);
void bad_ble_scene_start_on_exit(void* context);
void bad_ble_scene_start_submenu_callback(void* context, uint32_t index);

void bad_ble_scene_settings_on_enter(void* context);
bool bad_ble_scene_settings_on_event(void* context, SceneManagerEvent event);
void bad_ble_scene_settings_on_exit(void* context);
void bad_ble_scene_settings_var_item_callback(VariableItem* item);

void bad_ble_scene_run_on_enter(void* context);
bool bad_ble_scene_run_on_event(void* context, SceneManagerEvent event);
void bad_ble_scene_run_on_exit(void* context);

void bad_ble_scene_list_on_enter(void* context);
bool bad_ble_scene_list_on_event(void* context, SceneManagerEvent event);
void bad_ble_scene_list_on_exit(void* context);

bool bad_ble_back_event_callback(void* context);

extern const SceneManagerHandlers bad_ble_scene_handlers;

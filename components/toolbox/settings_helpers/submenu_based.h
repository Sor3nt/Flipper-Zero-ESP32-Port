#pragma once

#include <submenu.h>
#include <scene_manager.h>
#include <view_dispatcher.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SubmenuSettingsHelpherCallback)(void* context, uint32_t index);

typedef struct {
    const char* name;
    uint32_t scene_id;
} SubmenuSettingsHelperOption;

typedef struct {
    const char* app_name;
    size_t options_cnt;
    SubmenuSettingsHelperOption options[];
} SubmenuSettingsHelperDescriptor;

typedef struct SubmenuSettingsHelper SubmenuSettingsHelper;

SubmenuSettingsHelper*
    submenu_settings_helpers_alloc(const SubmenuSettingsHelperDescriptor* descriptor);

void submenu_settings_helpers_assign_objects(
    SubmenuSettingsHelper* helper,
    ViewDispatcher* view_dispatcher,
    SceneManager* scene_manager,
    Submenu* submenu,
    uint32_t submenu_view_id,
    uint32_t main_scene_id);

void submenu_settings_helpers_free(SubmenuSettingsHelper* helper);

bool submenu_settings_helpers_app_start(SubmenuSettingsHelper* helper, void* arg);

void submenu_settings_helpers_scene_enter(SubmenuSettingsHelper* helper);

bool submenu_settings_helpers_scene_event(SubmenuSettingsHelper* helper, SceneManagerEvent event);

void submenu_settings_helpers_scene_exit(SubmenuSettingsHelper* helper);

#ifdef __cplusplus
}
#endif

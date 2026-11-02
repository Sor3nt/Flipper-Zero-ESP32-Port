#include "../esp_flasher_app_i.h"

enum SubmenuIndex {
    SubmenuIndexEspFlasherQuickFlash,
    SubmenuIndexEspFlasherSwitchA,
    SubmenuIndexEspFlasherSwitchB,
    SubmenuIndexEspFlasherManualFlash,
    SubmenuIndexEspFlasherReset,
    SubmenuIndexEspFlasherBootloader,
    SubmenuIndexEspFlasherSettings,
    SubmenuIndexEspFlasherAbout,
};

static void esp_flasher_scene_start_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    EspFlasherApp* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void esp_flasher_scene_start_on_enter(void* context) {
    furi_assert(context);

    EspFlasherApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_set_header(submenu, "ESP Flasher");
    submenu_add_item(
        submenu,
        "Quick Flash",
        SubmenuIndexEspFlasherQuickFlash,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Manual Flash",
        SubmenuIndexEspFlasherManualFlash,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Switch to Firmware A",
        SubmenuIndexEspFlasherSwitchA,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Switch to Firmware B",
        SubmenuIndexEspFlasherSwitchB,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Reset Board",
        SubmenuIndexEspFlasherReset,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Enter Bootloader",
        SubmenuIndexEspFlasherBootloader,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Settings",
        SubmenuIndexEspFlasherSettings,
        esp_flasher_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "About",
        SubmenuIndexEspFlasherAbout,
        esp_flasher_scene_start_submenu_callback,
        app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, EspFlasherSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewSubmenu);
}

static void esp_flasher_scene_start_reset_job(EspFlasherApp* app) {
    app->reset = false;
    app->boot = false;
    app->quickflash = false;
    app->turbospeed = false;
    app->switch_fw = SwitchNotSet;
    app->expected_chip = ESP_UNKNOWN_CHIP;
}

bool esp_flasher_scene_start_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);

    EspFlasherApp* app = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        switch(event.event) {
        case SubmenuIndexEspFlasherAbout:
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneAbout);
            break;
        case SubmenuIndexEspFlasherSettings:
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneSettings);
            break;
        case SubmenuIndexEspFlasherQuickFlash:
            scene_manager_set_scene_state(app->scene_manager, EspFlasherSceneQuick, 0);
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneQuick);
            break;
        case SubmenuIndexEspFlasherSwitchA:
        case SubmenuIndexEspFlasherSwitchB:
            esp_flasher_scene_start_reset_job(app);
            app->boot = true;
            app->quickflash = true;
            app->switch_fw = event.event == SubmenuIndexEspFlasherSwitchA ? SwitchToFirmwareA :
                                                                            SwitchToFirmwareB;
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneConsoleOutput);
            break;
        case SubmenuIndexEspFlasherManualFlash:
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneBrowse);
            break;
        case SubmenuIndexEspFlasherReset:
            esp_flasher_scene_start_reset_job(app);
            app->reset = true;
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneConsoleOutput);
            break;
        case SubmenuIndexEspFlasherBootloader:
            esp_flasher_scene_start_reset_job(app);
            app->boot = true;
            scene_manager_next_scene(app->scene_manager, EspFlasherSceneConsoleOutput);
            break;
        default:
            // stray events (e.g. a late console refresh) must not move the selection
            consumed = false;
            break;
        }
        if(consumed) {
            scene_manager_set_scene_state(app->scene_manager, EspFlasherSceneStart, event.event);
        }
    }

    return consumed;
}

void esp_flasher_scene_start_on_exit(void* context) {
    furi_assert(context);

    EspFlasherApp* app = context;
    submenu_reset(app->submenu);
}

#include "../nrf24_app.h"
#include "../helpers/nrf24_mj_core.h"

#define MJ_TARGET_PICK_BASE 100

static void mj_target_list_callback(void* context, uint32_t index) {
    Nrf24App* app = context;
    if(index < MJ_TARGET_PICK_BASE) return;
    uint32_t i = index - MJ_TARGET_PICK_BASE;
    if(i >= app->mj_target_count) return;
    app->mj_selected_target = (int8_t)i;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void nrf24_app_scene_mj_target_list_on_enter(void* context) {
    Nrf24App* app = context;

    submenu_set_header(app->submenu, "Pick target");

    if(app->mj_target_count == 0) {
        widget_reset(app->widget);
        widget_add_text_box_element(
            app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, "\e#MouseJacker\e#", false);
        widget_add_text_box_element(
            app->widget, 0, 24, 128, 16, AlignCenter, AlignCenter, "No targets", false);
        widget_add_text_box_element(
            app->widget, 0, 44, 128, 12, AlignCenter, AlignCenter, "Back to retry", false);
        view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
        return;
    }

    char label[24];
    for(uint8_t i = 0; i < app->mj_target_count; i++) {
        mj_format_target(&app->mj_targets[i], label, sizeof(label));
        submenu_add_item(
            app->submenu, label, MJ_TARGET_PICK_BASE + i, mj_target_list_callback, app);
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

bool nrf24_app_scene_mj_target_list_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event >= MJ_TARGET_PICK_BASE) {
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneMjScriptSelect);
        return true;
    }
    return false;
}

void nrf24_app_scene_mj_target_list_on_exit(void* context) {
    Nrf24App* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
}

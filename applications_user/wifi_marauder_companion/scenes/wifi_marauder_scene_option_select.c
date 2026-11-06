#include "../wifi_marauder_app_i.h"

/* Option picker for a start-menu item, used on every board except the Paradoxt D-pad (see
 * wifi_marauder_scene_start.c). Picking an option stores it and returns to the start scene,
 * which runs the item; Back returns without running anything. */

static void wifi_marauder_scene_option_select_callback(void* context, uint32_t index) {
    WifiMarauderApp* app = context;
    const uint32_t item = (uint32_t)app->selected_menu_index;
    if((int)index >= wifi_marauder_scene_start_option_count(item)) return;

    app->selected_option_index[item] = (int)index;
    app->option_select_run = true;
    scene_manager_previous_scene(app->scene_manager);
}

void wifi_marauder_scene_option_select_on_enter(void* context) {
    WifiMarauderApp* app = context;
    Submenu* submenu = app->submenu;
    const uint32_t item = (uint32_t)app->selected_menu_index;

    app->option_select_run = false;
    submenu_set_header(submenu, wifi_marauder_scene_start_item_name(item));
    const int count = wifi_marauder_scene_start_option_count(item);
    for(int i = 0; i < count; i++) {
        submenu_add_item(
            submenu,
            wifi_marauder_scene_start_option_name(item, i),
            (uint32_t)i,
            wifi_marauder_scene_option_select_callback,
            app);
    }
    submenu_set_selected_item(submenu, (uint32_t)app->selected_option_index[item]);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiMarauderAppViewSubmenu);
}

bool wifi_marauder_scene_option_select_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void wifi_marauder_scene_option_select_on_exit(void* context) {
    WifiMarauderApp* app = context;
    submenu_reset(app->submenu);
}

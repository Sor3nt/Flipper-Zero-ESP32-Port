#include "../wifi_app.h"

enum EvilPortalTemplateIndex {
    EvilPortalTemplateIndexGoogle,
    EvilPortalTemplateIndexRouter,
    EvilPortalTemplateIndexSd,
};

static void evil_portal_template_cb(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_evil_portal_template_on_enter(void* context) {
    WifiApp* app = context;

    submenu_add_item(
        app->submenu, "Google Login", EvilPortalTemplateIndexGoogle, evil_portal_template_cb, app);
    submenu_add_item(
        app->submenu, "Router Update", EvilPortalTemplateIndexRouter, evil_portal_template_cb, app);
    submenu_add_item(
        app->submenu, "Custom (SD)", EvilPortalTemplateIndexSd, evil_portal_template_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_evil_portal_template_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EvilPortalTemplateIndexGoogle:
            app->evil_portal_template = WifiAppEvilPortalTemplateGoogle;
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
            break;
        case EvilPortalTemplateIndexRouter:
            app->evil_portal_template = WifiAppEvilPortalTemplateRouter;
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
            break;
        case EvilPortalTemplateIndexSd:
            app->evil_portal_template = WifiAppEvilPortalTemplateSd;
            strncpy(
                app->evil_portal_sd_path,
                "/ext/wifi/evil_portal/custom.html",
                sizeof(app->evil_portal_sd_path) - 1);
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_evil_portal_template_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}

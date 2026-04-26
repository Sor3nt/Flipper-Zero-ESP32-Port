#include "../wifi_app.h"
#include <stdio.h>
#include <string.h>

enum EvilPortalMenuIndex {
    EvilPortalMenuIndexSsid,
    EvilPortalMenuIndexChannel,
    EvilPortalMenuIndexTemplate,
    EvilPortalMenuIndexDeauth,
    EvilPortalMenuIndexStart,
};

static const uint8_t channel_steps[] = {1, 3, 6, 9, 11};

static const char* template_label(WifiAppEvilPortalTemplate t) {
    switch(t) {
    case WifiAppEvilPortalTemplateGoogle: return "Google";
    case WifiAppEvilPortalTemplateRouter: return "Router";
    case WifiAppEvilPortalTemplateSd: return "SD";
    default: return "?";
    }
}

static void evil_portal_menu_cb(void* context, uint32_t index) {
    WifiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wifi_app_scene_evil_portal_menu_on_enter(void* context) {
    WifiApp* app = context;

    if(app->evil_portal_ssid[0] == 0) {
        strcpy(app->evil_portal_ssid, "Free WiFi");
    }
    if(app->evil_portal_channel == 0) {
        app->evil_portal_channel = 6;
    }

    char label[64];

    snprintf(label, sizeof(label), "SSID: %s", app->evil_portal_ssid);
    submenu_add_item(app->submenu, label, EvilPortalMenuIndexSsid, evil_portal_menu_cb, app);

    snprintf(label, sizeof(label), "Channel: %u", app->evil_portal_channel);
    submenu_add_item(app->submenu, label, EvilPortalMenuIndexChannel, evil_portal_menu_cb, app);

    snprintf(label, sizeof(label), "Template: %s", template_label(app->evil_portal_template));
    submenu_add_item(app->submenu, label, EvilPortalMenuIndexTemplate, evil_portal_menu_cb, app);

    snprintf(label, sizeof(label), "Deauth: %s", app->evil_portal_deauth ? "On" : "Off");
    submenu_add_item(app->submenu, label, EvilPortalMenuIndexDeauth, evil_portal_menu_cb, app);

    submenu_add_item(app->submenu, "Start", EvilPortalMenuIndexStart, evil_portal_menu_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WifiAppViewSubmenu);
}

bool wifi_app_scene_evil_portal_menu_on_event(void* context, SceneManagerEvent event) {
    WifiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case EvilPortalMenuIndexSsid:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalSsid);
            consumed = true;
            break;
        case EvilPortalMenuIndexChannel: {
            size_t n = sizeof(channel_steps) / sizeof(channel_steps[0]);
            size_t cur = 0;
            for(size_t i = 0; i < n; i++) {
                if(channel_steps[i] == app->evil_portal_channel) {
                    cur = i;
                    break;
                }
            }
            cur = (cur + 1) % n;
            app->evil_portal_channel = channel_steps[cur];
            submenu_reset(app->submenu);
            wifi_app_scene_evil_portal_menu_on_enter(app);
            consumed = true;
            break;
        }
        case EvilPortalMenuIndexTemplate:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalTemplate);
            consumed = true;
            break;
        case EvilPortalMenuIndexDeauth:
            app->evil_portal_deauth = !app->evil_portal_deauth;
            submenu_reset(app->submenu);
            wifi_app_scene_evil_portal_menu_on_enter(app);
            consumed = true;
            break;
        case EvilPortalMenuIndexStart:
            scene_manager_next_scene(app->scene_manager, WifiAppSceneEvilPortalRun);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void wifi_app_scene_evil_portal_menu_on_exit(void* context) {
    WifiApp* app = context;
    submenu_reset(app->submenu);
}

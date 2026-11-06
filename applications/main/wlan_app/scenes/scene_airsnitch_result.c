#include "../wlan_app.h"

// AirSnitch-Schritt 3: read-only Ergebnis. Banner = Peer-Zähler, darunter die
// erreichbaren Geräte. KEINE Aktion pro Gerät (bewusst).

static void airsnitch_result_cb(void* context, uint32_t index) {
    UNUSED(context);
    UNUSED(index); // read-only: OK ohne Wirkung
}

static void airsnitch_format_ip(uint32_t ip_be, char* out, size_t cap) {
    const uint8_t* b = (const uint8_t*)&ip_be;
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void wlan_app_scene_airsnitch_result_on_enter(void* context) {
    WlanApp* app = context;

    submenu_reset(app->submenu);
    char header[32];
    snprintf(
        header, sizeof(header), "AirSnitch: %u peers", (unsigned)app->airsnitch_peer_count);
    submenu_set_header(app->submenu, header);

    if(app->airsnitch_peer_count == 0) {
        submenu_add_item(app->submenu, "No peers reachable", 0, airsnitch_result_cb, app);
    } else {
        for(uint8_t i = 0; i < app->airsnitch_peer_count; ++i) {
            WlanAirsnitchPeer* p = &app->airsnitch_peers[i];
            char ip[16];
            airsnitch_format_ip(p->ip, ip, sizeof(ip));
            char label[64];
            if(p->hostname[0]) {
                snprintf(
                    label, sizeof(label), "%s %s%s", ip, p->hostname,
                    p->same_subnet ? "" : " [priv]");
            } else {
                snprintf(
                    label, sizeof(label), "%s%s", ip, p->same_subnet ? "" : " [priv]");
            }
            submenu_add_item(app->submenu, label, i, airsnitch_result_cb, app);
        }
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_airsnitch_result_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WlanAppSceneMain);
        consumed = true;
    }

    return consumed;
}

void wlan_app_scene_airsnitch_result_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}

#include "../bw16_app_i.h"

/* Submenu item indices are offset so they never collide with anything else. */
#define BW16_AP_ITEM_BASE (1000u)

static void bw16_scene_ap_list_submenu_callback(void* context, uint32_t index) {
    Bw16App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* Builds the submenu from aps[]. Safe to read aps[] without the lock: it is only called
 * after LIST_STATION_DONE, at which point the worker has set capture_mode=None and no
 * longer touches the list (and the mutex acquire in the tick established visibility). */
static void bw16_scene_ap_list_build(Bw16App* app) {
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, "Select Network");

    if(app->ap_count == 0) {
        submenu_add_item(
            submenu,
            "No networks found",
            BW16_AP_ITEM_BASE,
            bw16_scene_ap_list_submenu_callback,
            app);
        return;
    }

    char label[64];
    for(size_t i = 0; i < app->ap_count; i++) {
        snprintf(
            label,
            sizeof(label),
            "%u.%s ch%d %ddBm",
            (unsigned)(i + 1),
            app->aps[i].ssid,
            app->aps[i].channel,
            app->aps[i].rssi);
        submenu_add_item(
            submenu,
            label,
            BW16_AP_ITEM_BASE + i,
            bw16_scene_ap_list_submenu_callback,
            app);
    }
}

void bw16_scene_ap_list_on_enter(void* context) {
    Bw16App* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Scanning APs...");
    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewSubmenu);

    /* Arm list capture under the lock, then request the station list. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->ap_count = 0;
    app->ap_list_done = false;
    app->capture_mode = Bw16CaptureAp;
    furi_mutex_release(app->mutex);

    bw16_uart_send_line(app->uart, "LIST_STATION");
}

bool bw16_scene_ap_list_on_event(void* context, SceneManagerEvent event) {
    Bw16App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        bool done = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->ap_list_done) {
            app->ap_list_done = false;
            done = true;
        }
        furi_mutex_release(app->mutex);
        if(done) bw16_scene_ap_list_build(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event >= BW16_AP_ITEM_BASE) {
            uint32_t idx = event.event - BW16_AP_ITEM_BASE;
            if(idx < app->ap_count) {
                app->selected_station = (int)idx;
                scene_manager_next_scene(app->scene_manager, Bw16SceneStationAction);
            }
        }
        consumed = true;
    }

    return consumed;
}

void bw16_scene_ap_list_on_exit(void* context) {
    Bw16App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->capture_mode = Bw16CaptureNone;
    furi_mutex_release(app->mutex);
    submenu_reset(app->submenu);
}

#include "../bw16_app_i.h"

#define BW16_CLIENT_ITEM_BASE (2000u)

static void bw16_scene_client_list_submenu_callback(void* context, uint32_t index) {
    Bw16App* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* Safe to read clients[] without the lock here (called only after LIST_CLIENTS_DONE). */
static void bw16_scene_client_list_build(Bw16App* app) {
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, "Select Client");

    if(app->client_count == 0) {
        submenu_add_item(
            submenu,
            "No clients found",
            BW16_CLIENT_ITEM_BASE,
            bw16_scene_client_list_submenu_callback,
            app);
        return;
    }

    char label[40];
    for(size_t i = 0; i < app->client_count; i++) {
        snprintf(label, sizeof(label), "%u.%s", (unsigned)(i + 1), app->clients[i]);
        submenu_add_item(
            submenu,
            label,
            BW16_CLIENT_ITEM_BASE + i,
            bw16_scene_client_list_submenu_callback,
            app);
    }
}

void bw16_scene_client_list_on_enter(void* context) {
    Bw16App* app = context;

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Scanning clients...");
    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewSubmenu);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->client_count = 0;
    app->client_list_done = false;
    app->capture_mode = Bw16CaptureClient;
    furi_mutex_release(app->mutex);

    char cmd[BW16_TX_CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "LIST_CLIENTS %d", app->selected_station);
    bw16_uart_send_line(app->uart, cmd);
}

bool bw16_scene_client_list_on_event(void* context, SceneManagerEvent event) {
    Bw16App* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        bool done = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->client_list_done) {
            app->client_list_done = false;
            done = true;
        }
        furi_mutex_release(app->mutex);
        if(done) bw16_scene_client_list_build(app);
        consumed = true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        if(event.event >= BW16_CLIENT_ITEM_BASE) {
            uint32_t idx = event.event - BW16_CLIENT_ITEM_BASE;
            if(idx < app->client_count) {
                app->selected_client = (int)idx;
                char cmd[BW16_TX_CMD_SIZE];
                snprintf(
                    cmd,
                    sizeof(cmd),
                    "DEAUTH_CLIENT %d %d",
                    app->selected_station,
                    app->selected_client);
                bw16_app_set_command(app, "CLIENT DEAUTH", cmd, true);
                scene_manager_next_scene(app->scene_manager, Bw16SceneConsole);
            }
        }
        consumed = true;
    }

    return consumed;
}

void bw16_scene_client_list_on_exit(void* context) {
    Bw16App* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->capture_mode = Bw16CaptureNone;
    furi_mutex_release(app->mutex);
    submenu_reset(app->submenu);
}

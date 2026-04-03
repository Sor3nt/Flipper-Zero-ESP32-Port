#include "esp_now_app.h"

static bool esp_now_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    EspNowApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool esp_now_app_back_event_callback(void* context) {
    furi_assert(context);
    EspNowApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void esp_now_app_tick_event_callback(void* context) {
    furi_assert(context);
    EspNowApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static EspNowApp* esp_now_app_alloc(void) {
    EspNowApp* app = malloc(sizeof(EspNowApp));

    app->gui = furi_record_open(RECORD_GUI);

    app->scene_manager = scene_manager_alloc(&esp_now_app_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, esp_now_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, esp_now_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, esp_now_app_tick_event_callback, 250);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, EspNowViewSubmenu, submenu_get_view(app->submenu));

    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, EspNowViewWidget, widget_get_view(app->widget));

    // Packet storage
    app->packet_capacity = ESP_NOW_PACKETS_MAX;
    app->packets = malloc(sizeof(EspNowPacket) * app->packet_capacity);
    app->packet_count = 0;
    app->last_displayed_count = 0;
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->text_buf = furi_string_alloc();

    return app;
}

static void esp_now_app_free(EspNowApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, EspNowViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, EspNowViewWidget);

    free(app->submenu);
    free(app->widget);

    free(app->scene_manager);
    free(app->view_dispatcher);

    furi_mutex_free(app->mutex);
    free(app->packets);
    furi_string_free(app->text_buf);

    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    free(app);
}

int32_t esp_now_app(void* args) {
    UNUSED(args);

    EspNowApp* app = esp_now_app_alloc();

    scene_manager_next_scene(app->scene_manager, EspNowAppSceneMenu);
    view_dispatcher_run(app->view_dispatcher);

    esp_now_app_free(app);
    return 0;
}

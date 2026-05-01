#include "ble_spam_app.h"
#include "ble_uuid_db.h"
#include "views/ble_spam_view.h"
#include "views/ble_walk_scan_view.h"
#include "views/ble_walk_detail_view.h"
#include "views/ble_auto_walk_view.h"
#include "views/tracker_list_view.h"
#include "views/tracker_geiger_view.h"

static bool ble_spam_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    BleSpamApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool ble_spam_back_event_callback(void* context) {
    furi_assert(context);
    BleSpamApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void ble_spam_tick_event_callback(void* context) {
    furi_assert(context);
    BleSpamApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static BleSpamApp* ble_spam_app_alloc(void) {
    BleSpamApp* app = malloc(sizeof(BleSpamApp));

    app->gui = furi_record_open(RECORD_GUI);

    app->scene_manager = scene_manager_alloc(&ble_spam_scene_handlers, app);
    app->view_dispatcher = view_dispatcher_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ble_spam_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, ble_spam_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, ble_spam_tick_event_callback, 250);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Submenu
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewSubmenu, submenu_get_view(app->submenu));

    // Running view
    app->view_running = ble_spam_view_alloc();
    view_set_context(app->view_running, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewRunning, app->view_running);

    // BLE Walk views
    app->view_walk_scan = ble_walk_scan_view_alloc();
    view_set_context(app->view_walk_scan, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewWalkScan, app->view_walk_scan);

    app->view_walk_detail = ble_walk_detail_view_alloc();
    view_set_context(app->view_walk_detail, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewWalkDetail, app->view_walk_detail);

    // BLE Auto-Walk view
    app->view_auto_walk = ble_auto_walk_view_alloc();
    view_set_context(app->view_auto_walk, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewAutoWalk, app->view_auto_walk);

    // BLE Tracker views
    app->view_tracker_scan = tracker_list_view_alloc();
    view_set_context(app->view_tracker_scan, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewTrackerScan, app->view_tracker_scan);

    app->view_tracker_geiger = tracker_geiger_view_alloc();
    view_set_context(app->view_tracker_geiger, app->view_dispatcher);
    view_dispatcher_add_view(
        app->view_dispatcher, BleSpamViewTrackerGeiger, app->view_tracker_geiger);

    app->tracker_geiger_timer = NULL;

    // State
    app->attack_type = BleSpamAttackAppleDevice;
    app->running = false;
    app->packet_count = 0;
    app->delay_ms = 100;
    app->current_index = 0;
    app->current_device[0] = '\0';

    return app;
}

static void ble_spam_app_free(BleSpamApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewRunning);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWalkScan);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewWalkDetail);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewAutoWalk);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewTrackerScan);
    view_dispatcher_remove_view(app->view_dispatcher, BleSpamViewTrackerGeiger);

    submenu_free(app->submenu);
    ble_spam_view_free(app->view_running);
    ble_walk_scan_view_free(app->view_walk_scan);
    ble_walk_detail_view_free(app->view_walk_detail);
    ble_auto_walk_view_free(app->view_auto_walk);
    tracker_list_view_free(app->view_tracker_scan);
    tracker_geiger_view_free(app->view_tracker_geiger);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    app->gui = NULL;

    free(app);
}

int32_t ble_spam_app(void* args) {
    UNUSED(args);
    ble_uuid_db_init();
    BleSpamApp* app = ble_spam_app_alloc();

    scene_manager_next_scene(app->scene_manager, BleSpamSceneMain);
    view_dispatcher_run(app->view_dispatcher);

    ble_spam_app_free(app);
    ble_uuid_db_deinit();
    return 0;
}

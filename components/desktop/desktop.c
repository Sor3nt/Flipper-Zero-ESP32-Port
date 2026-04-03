#include "desktop_i.h"

#include <gui.h>
#include <view_stack.h>

#include "scenes/desktop_scene.h"
#include "views/desktop_events.h"

#define TAG "Desktop"

static void desktop_loader_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const LoaderEvent* event = message;

    if(event->type == LoaderEventTypeApplicationBeforeLoad) {
        view_dispatcher_send_custom_event(
            desktop->view_dispatcher, DesktopGlobalBeforeAppStarted);
        furi_check(
            furi_semaphore_acquire(desktop->animation_semaphore, 3000) == FuriStatusOk);
    } else if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        view_dispatcher_send_custom_event(
            desktop->view_dispatcher, DesktopGlobalAfterAppFinished);
    }
}

static bool desktop_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;

    if(event == DesktopGlobalBeforeAppStarted) {
        if(animation_manager_is_animation_loaded(desktop->animation_manager)) {
            animation_manager_unload_and_stall_animation(desktop->animation_manager);
        }
        desktop->app_running = true;
        furi_semaphore_release(desktop->animation_semaphore);

    } else if(event == DesktopGlobalAfterAppFinished) {
        animation_manager_load_and_continue_animation(desktop->animation_manager);
        desktop->app_running = false;

    } else {
        return scene_manager_handle_custom_event(desktop->scene_manager, event);
    }

    return true;
}

static bool desktop_back_event_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;
    return scene_manager_handle_back_event(desktop->scene_manager);
}

static Desktop* desktop_alloc(void) {
    Desktop* desktop = malloc(sizeof(Desktop));
    memset(desktop, 0, sizeof(Desktop));

    FURI_LOG_I(TAG, "Allocating AnimationManager...");
    desktop->animation_semaphore = furi_semaphore_alloc(1, 0);
    desktop->animation_manager = animation_manager_alloc();
    FURI_LOG_I(TAG, "AnimationManager allocated OK");

    FURI_LOG_I(TAG, "Opening GUI record...");
    desktop->gui = furi_record_open(RECORD_GUI);
    FURI_LOG_I(TAG, "GUI record opened: %p", desktop->gui);

    desktop->view_dispatcher = view_dispatcher_alloc();
    desktop->scene_manager =
        scene_manager_alloc(&desktop_scene_handlers, desktop);

    view_dispatcher_attach_to_gui(
        desktop->view_dispatcher, desktop->gui, ViewDispatcherTypeDesktop);

    view_dispatcher_set_event_callback_context(desktop->view_dispatcher, desktop);
    view_dispatcher_set_custom_event_callback(
        desktop->view_dispatcher, desktop_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        desktop->view_dispatcher, desktop_back_event_callback);

    // Create main view stack: main_view (input) + animation (dolphin)
    desktop->main_view_stack = view_stack_alloc();
    desktop->main_view = desktop_main_alloc();
    desktop_main_set_dummy_mode_state(desktop->main_view, desktop->settings.dummy_mode);
    animation_manager_set_dummy_mode_state(
        desktop->animation_manager, desktop->settings.dummy_mode);

    View* dolphin_view =
        animation_manager_get_animation_view(desktop->animation_manager);

    view_stack_add_view(
        desktop->main_view_stack, desktop_main_get_view(desktop->main_view));
    view_stack_add_view(desktop->main_view_stack, dolphin_view);

    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdMain,
        view_stack_get_view(desktop->main_view_stack));

    // Subscribe to loader events for animation stall/continue
    desktop->loader = furi_record_open(RECORD_LOADER);
    furi_pubsub_subscribe(
        loader_get_pubsub(desktop->loader), desktop_loader_callback, desktop);

    desktop->app_running = loader_is_locked(desktop->loader);

    furi_record_create(RECORD_DESKTOP, desktop);

    FURI_LOG_I(TAG, "Desktop allocated successfully");
    return desktop;
}

int32_t desktop_srv(void* p) {
    UNUSED(p);

    FURI_LOG_I(TAG, "Starting Desktop service");

    Desktop* desktop = desktop_alloc();

    scene_manager_next_scene(desktop->scene_manager, DesktopSceneMain);

    // Special case: autostart application is already running
    if(desktop->app_running &&
       animation_manager_is_animation_loaded(desktop->animation_manager)) {
        animation_manager_unload_and_stall_animation(desktop->animation_manager);
    }

    FURI_LOG_I(TAG, "Desktop started, running view dispatcher");
    view_dispatcher_run(desktop->view_dispatcher);

    // Should never get here
    return 0;
}

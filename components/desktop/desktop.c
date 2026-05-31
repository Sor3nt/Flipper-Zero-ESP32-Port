#include "desktop_i.h"

#include <gui/gui_i.h>

#include <locale/locale.h>
#include <storage/storage.h>
#include <momentum/settings.h>

#include <assets_icons.h>

#include "scenes/desktop_scene.h"
#include "scenes/desktop_scene_locked.h"

#define TAG "Desktop"

static void desktop_auto_lock_arm(Desktop*);
static void desktop_auto_lock_inhibit(Desktop*);
static void desktop_start_auto_lock_timer(Desktop*);
static void desktop_apply_settings(Desktop*);

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

static void desktop_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalReloadSettings);
    }
}

static void desktop_lock_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_Lock_7x8);
}

static void desktop_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;

    if(event == DesktopGlobalBeforeAppStarted) {
        if(animation_manager_is_animation_loaded(desktop->animation_manager)) {
            animation_manager_unload_and_stall_animation(desktop->animation_manager);
        }

        desktop_auto_lock_inhibit(desktop);
        desktop->app_running = true;

        furi_semaphore_release(desktop->animation_semaphore);

    } else if(event == DesktopGlobalAfterAppFinished) {
        animation_manager_load_and_continue_animation(desktop->animation_manager);
        desktop_auto_lock_arm(desktop);
        desktop->app_running = false;

    } else if(event == DesktopGlobalAutoLock) {
        if(!desktop->app_running && !desktop->locked) {
            if(desktop->settings.usb_inhibit_auto_lock) {
                // ESP32: simplified USB check
            }
            desktop_lock(desktop, desktop->settings.auto_lock_with_pin);
        }

    } else if(event == DesktopGlobalSaveSettings) {
        desktop_settings_save(&desktop->settings);
        desktop_apply_settings(desktop);

    } else if(event == DesktopGlobalReloadSettings) {
        desktop_keybinds_migrate(desktop);
        desktop_settings_load(&desktop->settings);
        desktop_apply_settings(desktop);

    } else {
        scene_manager_handle_custom_event(desktop->scene_manager, event);
    }
}

static bool desktop_back_event_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;
    return scene_manager_handle_back_event(desktop->scene_manager);
}

static void desktop_tick_event_callback(void* context) {
    furi_assert(context);
    Desktop* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

static void desktop_auto_lock_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);
    UNUSED(value);
    Desktop* desktop = context;
    desktop_start_auto_lock_timer(desktop);
}

static void desktop_auto_lock_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalAutoLock);
}

static void desktop_start_auto_lock_timer(Desktop* desktop) {
    furi_timer_start(
        desktop->auto_lock_timer, furi_ms_to_ticks(desktop->settings.auto_lock_delay_ms));
}

static void desktop_stop_auto_lock_timer(Desktop* desktop) {
    furi_timer_stop(desktop->auto_lock_timer);
}

static void desktop_auto_lock_arm(Desktop* desktop) {
    if(desktop->settings.auto_lock_delay_ms) {
        if(!desktop->input_events_subscription) {
            desktop->input_events_subscription = furi_pubsub_subscribe(
                desktop->input_events_pubsub, desktop_auto_lock_callback, desktop);
        }
        desktop_start_auto_lock_timer(desktop);
    }
}

static void desktop_auto_lock_inhibit(Desktop* desktop) {
    desktop_stop_auto_lock_timer(desktop);
    if(desktop->input_events_subscription) {
        furi_pubsub_unsubscribe(
            desktop->input_events_pubsub, desktop->input_events_subscription);
        desktop->input_events_subscription = NULL;
    }
}

static void desktop_apply_settings(Desktop* desktop) {
    desktop->in_transition = true;

    if(!desktop->app_running && !desktop->locked) {
        desktop_auto_lock_arm(desktop);
    }

    view_port_enabled_set(desktop->lock_icon_viewport, desktop->locked);

    desktop->in_transition = false;
}

static void desktop_init_settings(Desktop* desktop) {
    furi_pubsub_subscribe(
        storage_get_pubsub(desktop->storage), desktop_storage_callback, desktop);

    if(storage_sd_status(desktop->storage) != FSE_OK) {
        FURI_LOG_D(TAG, "SD Card not ready, skipping settings");
        return;
    }

    desktop_keybinds_migrate(desktop);
    desktop_settings_load(&desktop->settings);
    desktop_apply_settings(desktop);
}

static Desktop* desktop_alloc(void) {
    Desktop* desktop = malloc(sizeof(Desktop));

    desktop->animation_semaphore = furi_semaphore_alloc(1, 0);
    desktop->animation_manager = animation_manager_alloc();
    desktop->gui = furi_record_open(RECORD_GUI);
    desktop->view_dispatcher = view_dispatcher_alloc();
    desktop->scene_manager =
        scene_manager_alloc(&desktop_scene_handlers, desktop);

    view_dispatcher_attach_to_gui(
        desktop->view_dispatcher, desktop->gui, ViewDispatcherTypeDesktop);
    view_dispatcher_set_tick_event_callback(
        desktop->view_dispatcher, desktop_tick_event_callback, 500);

    view_dispatcher_set_event_callback_context(desktop->view_dispatcher, desktop);
    view_dispatcher_set_custom_event_callback(
        desktop->view_dispatcher, desktop_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        desktop->view_dispatcher, desktop_back_event_callback);

    // Allocate views
    desktop->lock_menu = desktop_lock_menu_alloc();
    desktop->popup = popup_alloc();
    desktop->locked_view = desktop_view_locked_alloc();
    desktop->pin_input_view = desktop_view_pin_input_alloc();
    desktop->pin_timeout_view = desktop_view_pin_timeout_alloc();
    desktop->slideshow_view = desktop_view_slideshow_alloc();

    // Main view stack
    desktop->main_view_stack = view_stack_alloc();
    desktop->main_view = desktop_main_alloc();
    View* dolphin_view = animation_manager_get_animation_view(desktop->animation_manager);
    view_stack_add_view(desktop->main_view_stack, desktop_main_get_view(desktop->main_view));
    view_stack_add_view(desktop->main_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->main_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    // Locked view stack
    desktop->locked_view_stack = view_stack_alloc();
    view_stack_add_view(desktop->locked_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->locked_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    // Register views
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdMain,
        view_stack_get_view(desktop->main_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLocked,
        view_stack_get_view(desktop->locked_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLockMenu,
        desktop_lock_menu_get_view(desktop->lock_menu));
    view_dispatcher_add_view(
        desktop->view_dispatcher, DesktopViewIdPopup, popup_get_view(desktop->popup));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinTimeout,
        desktop_view_pin_timeout_get_view(desktop->pin_timeout_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinInput,
        desktop_view_pin_input_get_view(desktop->pin_input_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdSlideshow,
        desktop_view_slideshow_get_view(desktop->slideshow_view));

    // Lock icon viewport
    desktop->lock_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->lock_icon_viewport, icon_get_width(&I_Lock_7x8));
    view_port_draw_callback_set(
        desktop->lock_icon_viewport, desktop_lock_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->lock_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->lock_icon_viewport, GuiLayerStatusBarLeft);

    // Stealth mode icon viewport
    desktop->stealth_mode_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->stealth_mode_icon_viewport, icon_get_width(&I_Muted_8x8));
    view_port_enabled_set(
        desktop->stealth_mode_icon_viewport,
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode));
    gui_add_view_port(
        desktop->gui, desktop->stealth_mode_icon_viewport, GuiLayerStatusBarLeft);

    // Subscribe to loader events
    desktop->loader = furi_record_open(RECORD_LOADER);
    furi_pubsub_subscribe(
        loader_get_pubsub(desktop->loader), desktop_loader_callback, desktop);

    // Open storage and other records
    desktop->storage = furi_record_open(RECORD_STORAGE);
    desktop->notification = furi_record_open(RECORD_NOTIFICATION);
    desktop->input_events_pubsub = furi_record_open(RECORD_INPUT_EVENTS);

    // Auto-lock timer
    desktop->auto_lock_timer =
        furi_timer_alloc(desktop_auto_lock_timer_callback, FuriTimerTypeOnce, desktop);

    // Status pubsub
    desktop->status_pubsub = furi_pubsub_alloc();

    desktop->app_running = loader_is_locked(desktop->loader);

    furi_record_create(RECORD_DESKTOP, desktop);

    desktop->archive_dir = furi_string_alloc();

    return desktop;
}

/*
 * Private API
 */

void desktop_lock(Desktop* desktop, bool with_pin) {
    furi_assert(!desktop->locked);

    with_pin = with_pin && desktop_pin_code_is_set();
    if(with_pin) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagLock);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagLock);
        furi_hal_rtc_set_pin_fails(0);
    }

    desktop_auto_lock_inhibit(desktop);
    scene_manager_set_scene_state(
        desktop->scene_manager, DesktopSceneLocked, DesktopSceneLockedStateFirstEnter);
    scene_manager_next_scene(desktop->scene_manager, DesktopSceneLocked);

    view_port_enabled_set(desktop->lock_icon_viewport, true);

    desktop->locked = true;
}

void desktop_unlock(Desktop* desktop) {
    furi_assert(desktop->locked);

    view_port_enabled_set(desktop->lock_icon_viewport, false);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_set_lockdown(gui, false);
    furi_record_close(RECORD_GUI);
    desktop_view_locked_unlock(desktop->locked_view);
    scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
    desktop_auto_lock_arm(desktop);
    furi_hal_rtc_reset_flag(FuriHalRtcFlagLock);
    furi_hal_rtc_set_pin_fails(0);

    desktop->locked = false;
}

int32_t desktop_shutdown(void* context) {
    UNUSED(context);
    Power* power = furi_record_open(RECORD_POWER);
    power_off(power);
    furi_record_close(RECORD_POWER);
    return 0;
}

void desktop_set_stealth_mode_state(Desktop* desktop, bool enabled) {
    desktop->in_transition = true;

    if(enabled) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagStealthMode);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagStealthMode);
    }

    desktop_lock_menu_set_stealth_mode_state(desktop->lock_menu, enabled);
    view_port_enabled_set(desktop->stealth_mode_icon_viewport, enabled);

    desktop->in_transition = false;
}

void desktop_launch_archive(Desktop* desktop, const char* open_dir) {
    if(open_dir) {
        furi_string_set(desktop->archive_dir, open_dir);
    } else {
        furi_string_reset(desktop->archive_dir);
    }
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopMainEventOpenArchive);
}

/*
 * Public API
 */

bool desktop_api_is_locked(Desktop* instance) {
    furi_assert(instance);
    return furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock);
}

void desktop_api_unlock(Desktop* instance) {
    furi_assert(instance);
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalApiUnlock);
}

FuriPubSub* desktop_api_get_status_pubsub(Desktop* instance) {
    furi_assert(instance);
    return instance->status_pubsub;
}

void desktop_api_reload_settings(Desktop* instance) {
    furi_assert(instance);
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalReloadSettings);
}

void desktop_api_get_settings(Desktop* instance, DesktopSettings* settings) {
    furi_assert(instance);
    furi_assert(settings);
    *settings = instance->settings;
}

void desktop_api_set_settings(Desktop* instance, const DesktopSettings* settings) {
    furi_assert(instance);
    furi_assert(settings);
    instance->settings = *settings;
    view_dispatcher_send_custom_event(instance->view_dispatcher, DesktopGlobalSaveSettings);
}

/*
 * Service entry
 */

int32_t desktop_srv(void* p) {
    UNUSED(p);

    Desktop* desktop = desktop_alloc();

    desktop_init_settings(desktop);

    scene_manager_next_scene(desktop->scene_manager, DesktopSceneMain);

    // Lock on boot if PIN is set
    if(desktop_pin_code_is_set() && furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
        desktop_lock(desktop, true);
    }

    // Special case: autostart application is already running
    if(desktop->app_running &&
       animation_manager_is_animation_loaded(desktop->animation_manager)) {
        animation_manager_unload_and_stall_animation(desktop->animation_manager);
    }

    view_dispatcher_run(desktop->view_dispatcher);

    return 0;
}

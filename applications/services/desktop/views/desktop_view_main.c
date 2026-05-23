#include <gui/gui_i.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/canvas.h>
#include <furi.h>
#include <input/input.h>
#include <dolphin/dolphin.h>
#include <momentum/desktop_keybinds.h>

#include "desktop_view_main.h"

struct DesktopMainView {
    View* view;
    DesktopMainViewCallback callback;
    void* context;
    FuriTimer* poweroff_timer;
    bool dummy_mode;
    DesktopKeybinds* keybinds;
    Loader* loader;
};

#define DESKTOP_MAIN_VIEW_POWEROFF_TIMEOUT 1300

static void desktop_main_poweroff_timer_callback(void* context) {
    DesktopMainView* main_view = context;
    main_view->callback(DesktopMainEventOpenPowerOff, main_view->context);
}

void desktop_main_set_callback(
    DesktopMainView* main_view,
    DesktopMainViewCallback callback,
    void* context) {
    furi_assert(main_view);
    furi_assert(callback);
    main_view->callback = callback;
    main_view->context = context;
}

View* desktop_main_get_view(DesktopMainView* main_view) {
    furi_assert(main_view);
    return main_view->view;
}

void desktop_main_set_dummy_mode_state(DesktopMainView* main_view, bool dummy_mode) {
    furi_assert(main_view);
    main_view->dummy_mode = dummy_mode;
}

static void desktop_main_handle_keybind(DesktopMainView* main_view, InputKey key, InputType type) {
    if(!main_view->keybinds) return;

    DesktopKeybindType kb_type =
        (type == InputTypeLong) ? DesktopKeybindTypeHold : DesktopKeybindTypePress;
    const char* action = desktop_keybinds_get_action(main_view->keybinds, kb_type, key);

    if(action && strlen(action) > 0 && !loader_is_locked(main_view->loader)) {
        FuriString* error = furi_string_alloc();
        LoaderStatus status = loader_start(main_view->loader, action, NULL, error);
        if(status != LoaderStatusOk) {
            FURI_LOG_E("DesktopKeybinds", "Failed to start '%s': %s", action, furi_string_get_cstr(error));
        }
        furi_string_free(error);
        return;
    }

    // No keybind or locked — fall through to default behavior
    if(type == InputTypeShort) {
        switch(key) {
        case InputKeyOk:
            main_view->callback(DesktopMainEventOpenMenu, main_view->context);
            break;
        case InputKeyUp:
            main_view->callback(DesktopMainEventOpenLockMenu, main_view->context);
            break;
        case InputKeyDown:
            main_view->callback(DesktopMainEventOpenArchive, main_view->context);
            break;
        case InputKeyLeft:
            main_view->callback(DesktopMainEventOpenFavoriteLeftShort, main_view->context);
            break;
        default:
            break;
        }
    } else if(type == InputTypeLong) {
        switch(key) {
        case InputKeyUp:
            main_view->callback(DesktopMainEventLock, main_view->context);
            break;
        case InputKeyDown:
            main_view->callback(DesktopMainEventOpenDebug, main_view->context);
            break;
        case InputKeyLeft:
            main_view->callback(DesktopMainEventOpenFavoriteLeftLong, main_view->context);
            break;
        case InputKeyRight:
            main_view->callback(DesktopMainEventOpenFavoriteRightLong, main_view->context);
            break;
        case InputKeyOk: {
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
                main_view->callback(DesktopAnimationEventNewIdleAnimation, main_view->context);
            } else {
                main_view->callback(DesktopMainEventOpenFavoriteOkLong, main_view->context);
            }
            break;
        }
        default:
            break;
        }
    }
}

bool desktop_main_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopMainView* main_view = context;

    if(main_view->dummy_mode) {
        if(event->type == InputTypeShort) {
            switch(event->key) {
            case InputKeyOk:
                main_view->callback(DesktopDummyEventOpenOk, main_view->context);
                break;
            case InputKeyUp:
                main_view->callback(DesktopMainEventOpenLockMenu, main_view->context);
                break;
            case InputKeyDown:
                main_view->callback(DesktopDummyEventOpenDown, main_view->context);
                break;
            case InputKeyLeft:
                main_view->callback(DesktopDummyEventOpenLeft, main_view->context);
                break;
            default:
                break;
            }
        } else if(event->type == InputTypeLong) {
            switch(event->key) {
            case InputKeyOk:
                main_view->callback(DesktopDummyEventOpenOkLong, main_view->context);
                break;
            case InputKeyUp:
                main_view->callback(DesktopDummyEventOpenUpLong, main_view->context);
                break;
            case InputKeyDown:
                main_view->callback(DesktopDummyEventOpenDownLong, main_view->context);
                break;
            case InputKeyLeft:
                main_view->callback(DesktopDummyEventOpenLeftLong, main_view->context);
                break;
            case InputKeyRight:
                main_view->callback(DesktopDummyEventOpenRightLong, main_view->context);
                break;
            default:
                break;
            }
        }
    } else {
        desktop_main_handle_keybind(main_view, event->key, event->type);
    }

    if(event->key == InputKeyBack) {
        if(event->type == InputTypePress) {
            furi_timer_start(main_view->poweroff_timer, DESKTOP_MAIN_VIEW_POWEROFF_TIMEOUT);
        } else if(event->type == InputTypeRelease) {
            furi_timer_stop(main_view->poweroff_timer);
        }
    }

    return true;
}

DesktopMainView* desktop_main_alloc(void) {
    DesktopMainView* main_view = malloc(sizeof(DesktopMainView));

    main_view->view = view_alloc();
    view_set_context(main_view->view, main_view);
    view_set_input_callback(main_view->view, desktop_main_input_callback);

    main_view->poweroff_timer =
        furi_timer_alloc(desktop_main_poweroff_timer_callback, FuriTimerTypeOnce, main_view);

    main_view->keybinds = desktop_keybinds_alloc();
    desktop_keybinds_load(main_view->keybinds);
    main_view->loader = furi_record_open(RECORD_LOADER);

    return main_view;
}

void desktop_main_set_keybinds(DesktopMainView* main_view, DesktopKeybinds* keybinds) {
    furi_assert(main_view);
    if(main_view->keybinds) {
        desktop_keybinds_free(main_view->keybinds);
    }
    main_view->keybinds = keybinds;
}

void desktop_main_free(DesktopMainView* main_view) {
    furi_assert(main_view);
    view_free(main_view->view);
    furi_timer_free(main_view->poweroff_timer);
    if(main_view->keybinds) {
        desktop_keybinds_free(main_view->keybinds);
    }
    furi_record_close(RECORD_LOADER);
    free(main_view);
}

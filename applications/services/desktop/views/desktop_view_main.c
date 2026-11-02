#include <gui/gui_i.h>
#include <gui/view.h>
#include <gui/elements.h>
#include <gui/canvas.h>
#include <furi.h>
#include <input/input.h>
#include <dolphin/dolphin.h>

#include "desktop_view_main.h"

struct DesktopMainView {
    View* view;
    DesktopMainViewCallback callback;
    void* context;
    FuriTimer* poweroff_timer;
    FuriTimer* animation_timer;
    bool dummy_mode;
    bool select_long_pressed;
};

#define DESKTOP_MAIN_VIEW_POWEROFF_TIMEOUT 2500
#define DESKTOP_MAIN_VIEW_ANIMATION_TIMEOUT 2500

static void desktop_main_poweroff_timer_callback(void* context) {
    DesktopMainView* main_view = context;
    main_view->callback(DesktopMainEventOpenPowerOff, main_view->context);
}

static void desktop_main_animation_timer_callback(void* context) {
    DesktopMainView* main_view = context;
    main_view->callback(DesktopAnimationEventNewExternalAnimation, main_view->context);
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

bool desktop_main_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    DesktopMainView* main_view = context;

    esp_rom_printf("\r\n[DMAIN] key=%d type=%d dummy=%d\r\n",
        event->key, event->type, main_view->dummy_mode);

    if(event->key == InputKeyOk) {
        if(event->type == InputTypePress) {
            main_view->select_long_pressed = false;
            if(!main_view->dummy_mode) {
                furi_timer_start(main_view->animation_timer, DESKTOP_MAIN_VIEW_ANIMATION_TIMEOUT);
            }
        } else if(event->type == InputTypeShort) {
            furi_timer_stop(main_view->animation_timer);
            main_view->select_long_pressed = false;
        } else if(event->type == InputTypeRelease) {
            bool animation_timer_running = furi_timer_is_running(main_view->animation_timer);
            furi_timer_stop(main_view->animation_timer);
            if(main_view->select_long_pressed) {
                if(main_view->dummy_mode) {
                    main_view->callback(DesktopDummyEventOpenOkLong, main_view->context);
                } else if(animation_timer_running &&
                          furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug)) {
                    main_view->callback(
                        DesktopAnimationEventNewExternalAnimation, main_view->context);
                } else if(animation_timer_running) {
                    main_view->callback(
                        DesktopMainEventOpenFavoriteOkLong, main_view->context);
                }
            }
            main_view->select_long_pressed = false;
        } else if(event->type == InputTypeLong) {
            main_view->select_long_pressed = true;
        }
    }

    if(main_view->dummy_mode == false) {
        if(event->type == InputTypeShort) {
            if(event->key == InputKeyOk) {
                main_view->callback(DesktopMainEventOpenMenu, main_view->context);
            } else if(event->key == InputKeyUp) {
                main_view->callback(DesktopMainEventOpenLockMenu, main_view->context);
            } else if(event->key == InputKeyDown) {
                main_view->callback(DesktopMainEventOpenArchive, main_view->context);
            } else if(event->key == InputKeyLeft) {
                main_view->callback(DesktopMainEventOpenFavoriteLeftShort, main_view->context);
            }
            // Right key short is handled by animation manager
        } else if(event->type == InputTypeLong) {
            if(event->key == InputKeyUp) {
                main_view->callback(DesktopMainEventLock, main_view->context);
            } else if(event->key == InputKeyDown) {
                main_view->callback(DesktopMainEventOpenDebug, main_view->context);
            } else if(event->key == InputKeyLeft) {
                main_view->callback(DesktopMainEventOpenFavoriteLeftLong, main_view->context);
            } else if(event->key == InputKeyRight) {
                main_view->callback(DesktopMainEventOpenFavoriteRightLong, main_view->context);
            }
        }
    } else {
        if(event->type == InputTypeShort) {
            if(event->key == InputKeyOk) {
                main_view->callback(DesktopDummyEventOpenOk, main_view->context);
            } else if(event->key == InputKeyUp) {
                main_view->callback(DesktopMainEventOpenLockMenu, main_view->context);
            } else if(event->key == InputKeyDown) {
                main_view->callback(DesktopDummyEventOpenDown, main_view->context);
            } else if(event->key == InputKeyLeft) {
                main_view->callback(DesktopDummyEventOpenLeft, main_view->context);
            }
            // Right key short is handled by animation manager
        } else if(event->type == InputTypeLong) {
            if(event->key == InputKeyUp) {
                main_view->callback(DesktopDummyEventOpenUpLong, main_view->context);
            } else if(event->key == InputKeyDown) {
                main_view->callback(DesktopDummyEventOpenDownLong, main_view->context);
            } else if(event->key == InputKeyLeft) {
                main_view->callback(DesktopDummyEventOpenLeftLong, main_view->context);
            } else if(event->key == InputKeyRight) {
                main_view->callback(DesktopDummyEventOpenRightLong, main_view->context);
            }
        }
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
    main_view->animation_timer =
        furi_timer_alloc(desktop_main_animation_timer_callback, FuriTimerTypeOnce, main_view);

    return main_view;
}

void desktop_main_free(DesktopMainView* main_view) {
    furi_assert(main_view);
    furi_timer_free(main_view->animation_timer);
    view_free(main_view->view);
    furi_timer_free(main_view->poweroff_timer);
    free(main_view);
}

#include <furi.h>

#include <gui/elements.h>
#include <gui/icon.h>
#include <gui/view.h>
#include <furi_hal.h>
#include <stdio.h>

#include <assets_icons.h>
#include <locale/locale.h>
#include <momentum_settings.h>
#include <gui/modules/menu.h>

#include "../desktop_i.h"
#include "desktop_view_locked.h"

#define DOOR_MOVING_INTERVAL_MS  (1000 / 16)
#define LOCKED_HINT_TIMEOUT_MS   (1000)
#define UNLOCKED_HINT_TIMEOUT_MS (2000)
#define MOMENTUM_HINT_TIMEOUT_MS (3000)

#define DOOR_OFFSET_START (-55)
#define DOOR_OFFSET_END   (0)

#define DOOR_L_FINAL_POS (0)
#define DOOR_R_FINAL_POS (60)

#define UNLOCK_CNT         (3)
#define UNLOCK_RST_TIMEOUT (600)
#define COVER_MOVING_INTERVAL_MS (50)
#define COVER_OFFSET_START       (-64)
#define COVER_OFFSET_END         0

struct DesktopViewLocked {
    View* view;
    DesktopViewLockedCallback callback;
    void* context;

    FuriTimer* timer;
    uint8_t lock_count;
    uint32_t lock_lastpress;
};

typedef enum {
    DesktopViewLockedStateUnlocked,
    DesktopViewLockedStateLocked,
    DesktopViewLockedStateDoorsClosing,
    DesktopViewLockedStateCoverClosing,
    DesktopViewLockedStateCoverOpening,
    DesktopViewLockedStateLockedHintShown,
    DesktopViewLockedStateUnlockedHintShown
} DesktopViewLockedState;

typedef struct {
    bool pin_locked;
    int8_t door_offset;
    int8_t cover_offset;
    DesktopViewLockedState view_state;
} DesktopViewLockedModel;

void desktop_view_locked_set_callback(
    DesktopViewLocked* locked_view,
    DesktopViewLockedCallback callback,
    void* context) {
    furi_assert(locked_view);
    furi_assert(callback);
    locked_view->callback = callback;
    locked_view->context = context;
}

static void locked_view_timer_callback(void* context) {
    DesktopViewLocked* locked_view = context;
    locked_view->callback(DesktopLockedEventUpdate, locked_view->context);
}

static void desktop_view_locked_doors_draw(Canvas* canvas, DesktopViewLockedModel* model) {
    int32_t offset = model->door_offset;
    int32_t door_left_x = DOOR_L_FINAL_POS + offset;
    int32_t door_right_x = DOOR_R_FINAL_POS - offset;
    size_t height = icon_get_height(&I_DoorLeft_70x55);
    canvas_draw_icon(canvas, door_left_x, canvas_height(canvas) - height, &I_DoorLeft_70x55);
    canvas_draw_icon(canvas, door_right_x, canvas_height(canvas) - height, &I_DoorRight_70x55);
}

static void desktop_view_locked_draw_momentum(
    Canvas* canvas,
    const DesktopViewLockedModel* model) {
    MomentumSettings settings;
    momentum_settings_get_snapshot(&settings);
    int32_t y = model->cover_offset;
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);
    LocaleTimeFormat time_format = locale_get_time_format();
    LocaleDateFormat date_format = locale_get_date_format();
    char time_str[9];
    char second_str[5];
    char date_str[14];
    char meridian_str[3] = {0};
    bool pm = false;

    if(time_format == LocaleTimeFormat12h) {
        pm = datetime.hour > 12;
        snprintf(meridian_str, sizeof(meridian_str), "%s", datetime.hour >= 12 ? "PM" : "AM");
        if(datetime.hour == 0) datetime.hour = 12;
    }

    snprintf(
        time_str,
        sizeof(time_str),
        "%.2d:%.2d",
        pm ? datetime.hour - 12 : datetime.hour,
        datetime.minute);
    snprintf(second_str, sizeof(second_str), ":%.2d", datetime.second);

    if(date_format == LocaleDateFormatYMD) {
        snprintf(
            date_str, sizeof(date_str), "%.4d-%.2d-%.2d", datetime.year, datetime.month, datetime.day);
    } else if(date_format == LocaleDateFormatMDY) {
        snprintf(
            date_str, sizeof(date_str), "%.2d-%.2d-%.4d", datetime.month, datetime.day, datetime.year);
    } else {
        snprintf(
            date_str, sizeof(date_str), "%.2d-%.2d-%.4d", datetime.day, datetime.month, datetime.year);
    }

    if(!settings.lockscreen_transparent) {
        canvas_draw_icon(canvas, 0, y, &I_Lockscreen);
    }
    if(settings.lockscreen_time) {
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, 0, 64 + y, time_str);
        int32_t offset = canvas_string_width(canvas, time_str) + 2;
        if(settings.lockscreen_seconds) {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, offset, 64 + y, second_str);
            offset += canvas_string_width(canvas, ":00") + 2;
        }
        if(time_format == LocaleTimeFormat12h) {
            canvas_set_font(canvas, FontKeyboard);
            canvas_draw_str(canvas, offset, 64 + y, meridian_str);
        }
    }
    if(settings.lockscreen_date) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(
            canvas, 0, 48 + y + 16 * !settings.lockscreen_time, date_str);
    }
    if((model->view_state == DesktopViewLockedStateLockedHintShown || model->pin_locked) &&
       settings.lockscreen_prompt) {
        canvas_set_font(canvas, FontSecondary);
        if(model->pin_locked) {
            elements_bubble_str(
                canvas, 12, 14 + y, "  Press   \nto unlock!", AlignRight, AlignBottom);
            canvas_draw_icon(canvas, 45, 16 + y, &I_Pin_arrow_up_7x9);
        } else {
            elements_bubble_str(
                canvas, 2, 14 + y, "Press 3x      \n  to unlock!", AlignRight, AlignBottom);
            canvas_draw_icon(canvas, 43, 17 + y, &I_Pin_back_arrow_10x8);
        }
    }
}

static bool desktop_view_locked_cover_move(DesktopViewLockedModel* model, bool down) {
    if(down && model->cover_offset < COVER_OFFSET_END) {
        model->cover_offset = CLAMP(model->cover_offset + 8, COVER_OFFSET_END, COVER_OFFSET_START);
        return true;
    }
    if(!down && model->cover_offset > COVER_OFFSET_START) {
        model->cover_offset = CLAMP(model->cover_offset - 8, COVER_OFFSET_END, COVER_OFFSET_START);
        return true;
    }
    return false;
}

static bool desktop_view_locked_doors_move(DesktopViewLockedModel* model) {
    bool stop = false;
    if(model->door_offset < DOOR_OFFSET_END) {
        model->door_offset = CLAMP(model->door_offset + 5, DOOR_OFFSET_END, DOOR_OFFSET_START);
        stop = true;
    }

    return stop;
}

static void desktop_view_locked_update_hint_icon_timeout(DesktopViewLocked* locked_view) {
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    const bool change_state = (model->view_state == DesktopViewLockedStateLocked) &&
                              !model->pin_locked;
    if(change_state) {
        model->view_state = DesktopViewLockedStateLockedHintShown;
    }
    view_commit_model(locked_view->view, change_state);
    furi_timer_start(
        locked_view->timer,
        lock_screen_get_style() == LockScreenStyleMomentum ? MOMENTUM_HINT_TIMEOUT_MS :
                                                             LOCKED_HINT_TIMEOUT_MS);
}

void desktop_view_locked_update(DesktopViewLocked* locked_view) {
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    DesktopViewLockedState view_state = model->view_state;

    if(view_state == DesktopViewLockedStateDoorsClosing &&
       !desktop_view_locked_doors_move(model)) {
        locked_view->callback(DesktopLockedEventDoorsClosed, locked_view->context);
        model->view_state = DesktopViewLockedStateLocked;
    } else if(
        view_state == DesktopViewLockedStateCoverClosing &&
        !desktop_view_locked_cover_move(model, true)) {
        locked_view->callback(DesktopLockedEventDoorsClosed, locked_view->context);
        model->view_state = DesktopViewLockedStateLocked;
    } else if(
        view_state == DesktopViewLockedStateCoverOpening &&
        !desktop_view_locked_cover_move(model, false)) {
        model->view_state = DesktopViewLockedStateUnlockedHintShown;
        model->pin_locked = false;
        furi_timer_start(locked_view->timer, UNLOCKED_HINT_TIMEOUT_MS);
    } else if(view_state == DesktopViewLockedStateLockedHintShown) {
        model->view_state = DesktopViewLockedStateLocked;
    } else if(view_state == DesktopViewLockedStateUnlockedHintShown) {
        model->view_state = DesktopViewLockedStateUnlocked;
    }

    view_commit_model(locked_view->view, true);

    if(model->view_state == DesktopViewLockedStateDoorsClosing ||
       model->view_state == DesktopViewLockedStateCoverClosing ||
       model->view_state == DesktopViewLockedStateCoverOpening) {
        furi_timer_start(
            locked_view->timer,
            model->view_state == DesktopViewLockedStateDoorsClosing ? DOOR_MOVING_INTERVAL_MS :
                                                                       COVER_MOVING_INTERVAL_MS);
    } else if(
        model->view_state == DesktopViewLockedStateLocked &&
        lock_screen_get_style() == LockScreenStyleMomentum) {
        MomentumSettings settings;
        momentum_settings_get_snapshot(&settings);
        if(settings.lockscreen_time || settings.lockscreen_date) {
            furi_timer_start(locked_view->timer, settings.lockscreen_seconds ? 1000 : 60000);
        } else {
            furi_timer_stop(locked_view->timer);
        }
    } else if(model->view_state == DesktopViewLockedStateUnlockedHintShown) {
        furi_timer_start(locked_view->timer, UNLOCKED_HINT_TIMEOUT_MS);
    } else {
        furi_timer_stop(locked_view->timer);
    }
}

static void desktop_view_locked_draw(Canvas* canvas, void* model) {
    DesktopViewLockedModel* m = model;
    DesktopViewLockedState view_state = m->view_state;
    canvas_set_color(canvas, ColorBlack);

    if(lock_screen_get_style() == LockScreenStyleMomentum &&
       (view_state == DesktopViewLockedStateLocked ||
        view_state == DesktopViewLockedStateLockedHintShown ||
        view_state == DesktopViewLockedStateCoverClosing ||
        view_state == DesktopViewLockedStateCoverOpening)) {
        desktop_view_locked_draw_momentum(canvas, m);
    } else if(view_state == DesktopViewLockedStateDoorsClosing) {
        desktop_view_locked_doors_draw(canvas, m);
        canvas_set_font(canvas, FontPrimary);
        elements_multiline_text_framed(canvas, 42, 30 + STATUS_BAR_Y_SHIFT, "Locked");
    } else if(view_state == DesktopViewLockedStateLockedHintShown) {
        canvas_set_font(canvas, FontSecondary);
        elements_bold_rounded_frame(canvas, 14, 2 + STATUS_BAR_Y_SHIFT, 99, 48);
        elements_multiline_text(canvas, 65, 20 + STATUS_BAR_Y_SHIFT, "To unlock\npress:");
        canvas_draw_icon(canvas, 65, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 80, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 95, 36 + STATUS_BAR_Y_SHIFT, &I_Pin_back_arrow_10x8);
        canvas_draw_icon(canvas, 16, 7 + STATUS_BAR_Y_SHIFT, &I_WarningDolphin_45x42);
        canvas_draw_dot(canvas, 17, 61);
    } else if(view_state == DesktopViewLockedStateUnlockedHintShown) {
        canvas_set_font(canvas, FontPrimary);
        elements_multiline_text_framed(canvas, 42, 30 + STATUS_BAR_Y_SHIFT, "Unlocked");
    }
}

View* desktop_view_locked_get_view(DesktopViewLocked* locked_view) {
    furi_assert(locked_view);
    return locked_view->view;
}

static bool desktop_view_locked_input(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);

    bool is_changed = false;
    const uint32_t press_time = furi_get_tick();
    DesktopViewLocked* locked_view = context;
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    const bool momentum_style = lock_screen_get_style() == LockScreenStyleMomentum;
    if(model->view_state == DesktopViewLockedStateUnlockedHintShown &&
       event->type == InputTypePress) {
        model->view_state = DesktopViewLockedStateUnlocked;
        is_changed = true;
    }
    const DesktopViewLockedState view_state = model->view_state;
    const bool pin_locked = model->pin_locked;
    view_commit_model(locked_view->view, is_changed);

    if(view_state == DesktopViewLockedStateUnlocked) {
        return false;
    } else if(
        (view_state == DesktopViewLockedStateLocked ||
         view_state == DesktopViewLockedStateLockedHintShown) &&
        pin_locked) {
        if(event->key == InputKeyBack && event->type == InputTypeLong) {
            locked_view->callback(DesktopLockedEventOpenPowerOff, locked_view->context);
        } else if(
            !momentum_style ||
            (event->key == InputKeyUp && event->type == InputTypeShort)) {
            locked_view->callback(DesktopLockedEventShowPinInput, locked_view->context);
        }
    } else if(
        view_state == DesktopViewLockedStateLocked ||
        view_state == DesktopViewLockedStateLockedHintShown) {
        if(press_time - locked_view->lock_lastpress > UNLOCK_RST_TIMEOUT) {
            locked_view->lock_lastpress = press_time;
            locked_view->lock_count = 0;
        }

        desktop_view_locked_update_hint_icon_timeout(locked_view);

        if(event->key == InputKeyBack && event->type == InputTypeLong) {
            locked_view->callback(DesktopLockedEventOpenPowerOff, locked_view->context);
            locked_view->lock_count = 0;
        } else if(event->key == InputKeyBack) {
            if(event->type == InputTypeShort) {
                locked_view->lock_lastpress = press_time;
                locked_view->lock_count++;
                if(locked_view->lock_count == UNLOCK_CNT) {
                    locked_view->callback(DesktopLockedEventUnlocked, locked_view->context);
                }
            }
        } else {
            locked_view->lock_count = 0;
        }

        locked_view->lock_lastpress = press_time;
    }

    return true;
}

DesktopViewLocked* desktop_view_locked_alloc(void) {
    DesktopViewLocked* locked_view = malloc(sizeof(DesktopViewLocked));
    locked_view->view = view_alloc();
    locked_view->timer =
        furi_timer_alloc(locked_view_timer_callback, FuriTimerTypePeriodic, locked_view);

    view_allocate_model(locked_view->view, ViewModelTypeLocking, sizeof(DesktopViewLockedModel));
    view_set_context(locked_view->view, locked_view);
    view_set_draw_callback(locked_view->view, desktop_view_locked_draw);
    view_set_input_callback(locked_view->view, desktop_view_locked_input);

    return locked_view;
}

void desktop_view_locked_free(DesktopViewLocked* locked_view) {
    furi_assert(locked_view);
    furi_timer_free(locked_view->timer);
    view_free(locked_view->view);
    free(locked_view);
}

void desktop_view_locked_close_doors(DesktopViewLocked* locked_view) {
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    furi_assert(model->view_state == DesktopViewLockedStateLocked);
    if(lock_screen_get_style() == LockScreenStyleMomentum) {
        if(momentum_settings_get_bool(MomentumSettingsBoolLockscreenSkipAnimation)) {
            model->cover_offset = COVER_OFFSET_END;
            view_commit_model(locked_view->view, true);
            locked_view->callback(DesktopLockedEventDoorsClosed, locked_view->context);
        } else {
            model->view_state = DesktopViewLockedStateCoverClosing;
            model->cover_offset = COVER_OFFSET_START;
            view_commit_model(locked_view->view, true);
            furi_timer_start(locked_view->timer, COVER_MOVING_INTERVAL_MS);
        }
        return;
    }

    model->view_state = DesktopViewLockedStateDoorsClosing;
    model->door_offset = DOOR_OFFSET_START;
    view_commit_model(locked_view->view, true);
    furi_timer_start(locked_view->timer, DOOR_MOVING_INTERVAL_MS);
}

void desktop_view_locked_lock(DesktopViewLocked* locked_view, bool pin_locked) {
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    furi_assert(model->view_state == DesktopViewLockedStateUnlocked);
    model->view_state = DesktopViewLockedStateLocked;
    model->pin_locked = pin_locked;
    view_commit_model(locked_view->view, true);
}

void desktop_view_locked_unlock(DesktopViewLocked* locked_view) {
    locked_view->lock_count = 0;
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    if(lock_screen_get_style() == LockScreenStyleMomentum) {
        model->pin_locked = false;
        if(momentum_settings_get_bool(MomentumSettingsBoolLockscreenSkipAnimation)) {
            model->view_state = DesktopViewLockedStateUnlocked;
            model->cover_offset = COVER_OFFSET_START;
            view_commit_model(locked_view->view, true);
        } else {
            model->view_state = DesktopViewLockedStateCoverOpening;
            model->cover_offset = COVER_OFFSET_END;
            view_commit_model(locked_view->view, true);
            furi_timer_start(locked_view->timer, COVER_MOVING_INTERVAL_MS);
        }
        return;
    }

    model->view_state = DesktopViewLockedStateUnlockedHintShown;
    model->pin_locked = false;
    view_commit_model(locked_view->view, true);
    furi_timer_start(locked_view->timer, UNLOCKED_HINT_TIMEOUT_MS);
}

bool desktop_view_locked_is_locked_hint_visible(DesktopViewLocked* locked_view) {
    DesktopViewLockedModel* model = view_get_model(locked_view->view);
    const DesktopViewLockedState view_state = model->view_state;
    view_commit_model(locked_view->view, false);
    return view_state == DesktopViewLockedStateLockedHintShown ||
           view_state == DesktopViewLockedStateLocked;
}

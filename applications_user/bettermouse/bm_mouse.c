#include "bm_mouse.h"

#include <furi_hal_usb_hid.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <math.h>
#include <furi_hal_board_info.h> // PORT: encoder input path

/* ESP32 port note: the upstream draws the mouse screen with app-bundled icons
   (bettermouse_icons.h / assets). To keep the FAP self-contained (no icon
   pipeline), the draw callback below is redrawn with canvas primitives. The
   motion engine, input handling and HID below are the upstream verbatim. */

#define BM_DIAG       0.70710678f /* keep diagonals the same speed as axes */
#define BM_EXIT_HOLD  800u /* ms holding Back alone before we drop to the menu */
#define BM_REPORT_MAX 127 /* HID reports are int8 */
#define BM_ICE_STOP   8.0f /* px/s below which a glide is over */
#define BM_BAR_MIN_MS 250u /* min gap between speed bar repaints */

#define BM_FLAG_STOP (1u << 0)

/* PORT: rotary-encoder boards (T-Embed, ThorChimera). A wheel detent is an instant
   Press/Short/Release, so the held-key motion engine below almost never sees it. On the
   encoder every detent is an impulse of BM_ENC_STEP_PX pixels instead, multiplied when the
   wheel is spun fast (detents closer than BM_ENC_FAST2_MS / BM_ENC_FAST4_MS). */
#define BM_ENC_STEP_PX  6.0f
#define BM_ENC_FAST2_MS 80u
#define BM_ENC_FAST4_MS 35u

/* Only what the screen shows. Written by the motion thread, read by the draw
   callback, and deliberately updated as rarely as possible. */
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool back_down;
    bool scroll_mode;
    bool drag_lock;
    bool btn_left;
    bool btn_right;
    bool connected;
    bool ice;
    bool gliding;
    bool exit_warn; /* Back held alone, quit is coming */
    uint8_t speed_pct;
} BmMouseModel;

/* Live key state. Plain bytes and counters, written on the input service thread
   and read by the motion thread; no lock, so neither can ever stall the other.
   Byte-sized loads and stores are atomic. */
typedef struct {
    volatile bool up;
    volatile bool down;
    volatile bool left;
    volatile bool right;

    volatile bool back_held;
    volatile bool back_consumed; /* Back already did something, so no right click */
    volatile bool ok_held;
    volatile bool ok_swallowed; /* this OK press already did something else */
    volatile bool drag_lock; /* owned by the motion thread, read here */

    /* Requests handed to the motion thread, which owns all USB traffic. The
       input service thread must never block on a HID report.
       PORT: running totals written ONLY by the input thread (it increments them);
       the motion thread keeps how many it has served in BmMouse::req_done and never
       writes these. The old ++ here / -- there was a read-modify-write from two
       threads (on two cores of the S3) that could lose or repeat a click. */
    volatile uint8_t click_left;
    volatile uint8_t click_right;
    volatile uint8_t toggle_scroll;
    volatile uint8_t drag_press;
    volatile uint8_t drag_release;

    /* PORT: encoder impulses. Running totals of detent units (sign = direction, a fast spin
       counts 2 or 4 per detent), written only by the input thread; the motion thread keeps
       what it has already consumed in BmMouse, so no read-modify-write is shared. */
    volatile int32_t enc_x_units;
    volatile int32_t enc_y_units;
    uint32_t enc_last_tick; /* input thread only */
} BmInput;

struct BmMouse {
    View* view;
    const BmSettings* settings;
    NotificationApp* notifications;

    FuriThread* thread;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;

    BmMouseExitCallback exit_callback;
    void* exit_context;

    BmInput input;

    /* Owned by the motion thread. */
    float vx; /* px/s, only meaningful in ice mode */
    float vy;
    float acc_x; /* sub-pixel remainders */
    float acc_y;
    float acc_scroll;
    uint32_t hold_ms;
    uint32_t back_hold_ms;
    bool scroll_mode;
    bool exit_requested;

    /* PORT: encoder board flag and the impulse totals already consumed (motion thread). */
    bool is_encoder;
    int32_t enc_x_done;
    int32_t enc_y_done;

    /* PORT: request totals already served (motion thread only), see BmInput. */
    struct {
        uint8_t click_left;
        uint8_t click_right;
        uint8_t toggle_scroll;
        uint8_t drag_press;
        uint8_t drag_release;
    } req_done;
};

/* ------------------------------------------------------------------ drawing */

/* A small direction arrow drawn as a triangle; filled when the key is held. */
static void bm_draw_arrow(Canvas* canvas, uint8_t cx, uint8_t cy, char dir, bool on) {
    const uint8_t s = 4; /* half-size */
    switch(dir) {
    case 'U':
        canvas_draw_line(canvas, cx, cy - s, cx - s, cy + s);
        canvas_draw_line(canvas, cx, cy - s, cx + s, cy + s);
        if(on) canvas_draw_line(canvas, cx - s, cy + s, cx + s, cy + s);
        break;
    case 'D':
        canvas_draw_line(canvas, cx, cy + s, cx - s, cy - s);
        canvas_draw_line(canvas, cx, cy + s, cx + s, cy - s);
        if(on) canvas_draw_line(canvas, cx - s, cy - s, cx + s, cy - s);
        break;
    case 'L':
        canvas_draw_line(canvas, cx - s, cy, cx + s, cy - s);
        canvas_draw_line(canvas, cx - s, cy, cx + s, cy + s);
        if(on) canvas_draw_line(canvas, cx + s, cy - s, cx + s, cy + s);
        break;
    case 'R':
        canvas_draw_line(canvas, cx + s, cy, cx - s, cy - s);
        canvas_draw_line(canvas, cx + s, cy, cx - s, cy + s);
        if(on) canvas_draw_line(canvas, cx - s, cy - s, cx - s, cy + s);
        break;
    default:
        break;
    }
}

static void bm_draw_button(Canvas* canvas, uint8_t x, uint8_t y, const char* label, bool pressed) {
    if(pressed) {
        canvas_draw_rbox(canvas, x, y, 16, 11, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, x + 8, y + 2, AlignCenter, AlignTop, label);
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_rframe(canvas, x, y, 16, 11, 2);
        canvas_draw_str_aligned(canvas, x + 8, y + 2, AlignCenter, AlignTop, label);
    }
}

static void bm_mouse_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    BmMouseModel* model = context;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "BMouse");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 22, model->connected ? "USB ready" : "No USB host");

    const char* mode = "Pointer";
    if(model->scroll_mode) {
        mode = "Scroll";
    } else if(model->ice) {
        mode = model->gliding ? "Ice: glide" : "Ice";
    }
    canvas_draw_str(canvas, 0, 33, mode);

    if(model->drag_lock) canvas_draw_str(canvas, 0, 44, "DRAG");

    /* Speed bar: where you are on the ramp. */
    canvas_draw_frame(canvas, 0, 47, 52, 5);
    if(model->speed_pct > 0) {
        uint8_t w = (uint8_t)((50u * model->speed_pct) / 100u);
        if(w > 0) canvas_draw_box(canvas, 1, 48, w, 3);
    }

    canvas_draw_str(canvas, 0, 63, model->exit_warn ? "MENU..." : "Hold Bk=menu");

    /* Right side: a D-pad of direction arrows, highlighting the held keys. */
    const uint8_t cx = 96;
    const uint8_t cy = 20;
    canvas_draw_circle(canvas, cx, cy, 2);
    bm_draw_arrow(canvas, cx, (uint8_t)(cy - 11), 'U', model->up);
    bm_draw_arrow(canvas, cx, (uint8_t)(cy + 11), 'D', model->down);
    bm_draw_arrow(canvas, (uint8_t)(cx - 13), cy, 'L', model->left);
    bm_draw_arrow(canvas, (uint8_t)(cx + 13), cy, 'R', model->right);

    /* Mouse buttons. OK = left (also lit during a drag lock), Back = right. */
    bm_draw_button(canvas, 78, 38, "L", model->btn_left);
    bm_draw_button(canvas, 102, 38, "R", model->btn_right);
}

/* ---------------------------------------------------------------- input tap */

/* PORT: encoder key map. Returns true when the event was handled here; Back (right click,
   menu hold) falls through to the upstream path below.
     wheel (Up/Down)              pointer: move vertically; scroll mode: scroll
     OK held + turn (Left/Right)  move horizontally
     OK click                     left click (or drop a drag lock)
     OK hold (no turn)            drag lock
   The encoder driver also emits an OK Press at the start of every OK-held+turn and in front of
   OK Long, and only a Release when a held OK is let go, so upstream's "click on Release" fired a
   spurious left click after every horizontal move. Here only OK Short clicks. */
static bool bm_mouse_input_event_encoder(const InputEvent* event, BmMouse* bm_mouse) {
    BmInput* in = &bm_mouse->input;

    switch(event->key) {
    case InputKeyUp:
    case InputKeyDown:
    case InputKeyLeft:
    case InputKeyRight:
        if(event->type == InputTypeShort) {
            uint32_t now = furi_get_tick();
            uint32_t gap = now - in->enc_last_tick;
            in->enc_last_tick = now;
            int32_t units = 1;
            if(gap < furi_ms_to_ticks(BM_ENC_FAST4_MS)) {
                units = 4;
            } else if(gap < furi_ms_to_ticks(BM_ENC_FAST2_MS)) {
                units = 2;
            }
            if(event->key == InputKeyUp) in->enc_y_units = in->enc_y_units - units;
            if(event->key == InputKeyDown) in->enc_y_units = in->enc_y_units + units;
            if(event->key == InputKeyLeft) in->enc_x_units = in->enc_x_units - units;
            if(event->key == InputKeyRight) in->enc_x_units = in->enc_x_units + units;
        }
        return true; /* never feed the held-key engine */

    case InputKeyOk:
        if(event->type == InputTypePress) {
            if(in->back_held) {
                /* Back held + OK toggles scroll mode, as upstream. */
                in->back_consumed = true;
                in->ok_swallowed = true;
                in->toggle_scroll++;
            } else if(!in->ok_held) {
                in->ok_held = true;
                in->ok_swallowed = false;
            }
        } else if(event->type == InputTypeLong) {
            /* Drag lock only for a plain hold: a Long right after an OK-held+turn detent is
               the end of that gesture, not a request to drag. */
            bool turned = (furi_get_tick() - in->enc_last_tick) < furi_ms_to_ticks(600);
            if(!in->back_held && !in->ok_swallowed && !turned && !in->drag_lock) {
                in->drag_press++;
            }
            in->ok_swallowed = true;
        } else if(event->type == InputTypeShort) {
            if(!in->ok_swallowed) {
                if(in->drag_lock) {
                    in->drag_release++;
                } else {
                    in->click_left++;
                }
            }
            in->ok_swallowed = true;
        } else if(event->type == InputTypeRelease) {
            in->ok_held = false;
        }
        return true;

    default:
        return false;
    }
}

static void bm_mouse_input_event(const void* value, void* context) {
    const InputEvent* event = value;
    BmMouse* bm_mouse = context;
    BmInput* in = &bm_mouse->input;

    if(bm_mouse->is_encoder && bm_mouse_input_event_encoder(event, bm_mouse)) return;

    switch(event->key) {
    case InputKeyUp:
    case InputKeyDown:
    case InputKeyLeft:
    case InputKeyRight: {
        if(event->type != InputTypePress && event->type != InputTypeRelease) break;
        bool pressed = (event->type == InputTypePress);

        if(event->key == InputKeyUp) in->up = pressed;
        if(event->key == InputKeyDown) in->down = pressed;
        if(event->key == InputKeyLeft) in->left = pressed;
        if(event->key == InputKeyRight) in->right = pressed;

        break;
    }

    case InputKeyOk:
        if(event->type == InputTypePress) {
            if(in->back_held) {
                in->back_consumed = true;
                in->ok_swallowed = true;
                in->toggle_scroll++;
            } else {
                in->ok_held = true;
                in->ok_swallowed = false;
            }
        } else if(event->type == InputTypeLong) {
            if(!in->back_held && !in->drag_lock && !in->ok_swallowed) in->drag_press++;
        } else if(event->type == InputTypeShort) {
            /* Short arrives before Release, so dropping the latch here means
               the Release below correctly does not also fire a click. */
            if(in->drag_lock) {
                in->drag_release++;
                in->ok_swallowed = true;
            }
        } else if(event->type == InputTypeRelease) {
            if(in->ok_held && !in->ok_swallowed && !in->drag_lock) in->click_left++;
            in->ok_held = false;
        }
        break;

    case InputKeyBack:
        if(event->type == InputTypePress) {
            in->back_held = true;
            in->back_consumed = false;
        } else if(event->type == InputTypeRelease) {
            bool consumed = in->back_consumed;
            in->back_held = false;
            if(!consumed) in->click_right++;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------ motion engine */

static int8_t bm_take_whole(float* accumulator) {
    /* Truncate toward zero and keep the remainder, so speeds below one pixel
       per tick still move the pointer instead of rounding away to nothing. */
    float whole = (*accumulator < 0.0f) ? ceilf(*accumulator) : floorf(*accumulator);

    /* Clamp to the int8 report range, leaving anything over in the accumulator
       so a delayed tick spreads over the next few instead of being dropped. */
    if(whole > (float)BM_REPORT_MAX) whole = (float)BM_REPORT_MAX;
    if(whole < (float)-BM_REPORT_MAX) whole = (float)-BM_REPORT_MAX;

    *accumulator -= whole;
    return (int8_t)whole;
}

static void bm_mouse_vibro(BmMouse* bm_mouse) {
    if(bm_mouse->settings->haptic) {
        notification_message(bm_mouse->notifications, &sequence_single_vibro);
    }
}

/* PORT: how many requests arrived since the last call. The input thread is the
   only writer of *total; uint8_t wrap-around keeps the difference right. */
static uint8_t bm_take_requests(const volatile uint8_t* total, uint8_t* done) {
    const uint8_t now = *total;
    const uint8_t count = (uint8_t)(now - *done);
    *done = now;
    return count;
}

/* Act on everything the input thread asked for. All USB traffic happens here. */
static void bm_mouse_service_requests(BmMouse* bm_mouse, bool connected) {
    BmInput* in = &bm_mouse->input;

    for(uint8_t n = bm_take_requests(&in->click_left, &bm_mouse->req_done.click_left); n > 0;
        n--) {
        if(connected) {
            furi_hal_hid_mouse_press(HID_MOUSE_BTN_LEFT);
            furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        }
        bm_mouse_vibro(bm_mouse);
    }

    for(uint8_t n = bm_take_requests(&in->click_right, &bm_mouse->req_done.click_right); n > 0;
        n--) {
        if(connected) {
            furi_hal_hid_mouse_press(HID_MOUSE_BTN_RIGHT);
            furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
        }
        bm_mouse_vibro(bm_mouse);
    }

    for(uint8_t n = bm_take_requests(&in->drag_press, &bm_mouse->req_done.drag_press); n > 0;
        n--) {
        if(connected) furi_hal_hid_mouse_press(HID_MOUSE_BTN_LEFT);
        in->drag_lock = true;
        bm_mouse_vibro(bm_mouse);
    }

    for(uint8_t n = bm_take_requests(&in->drag_release, &bm_mouse->req_done.drag_release);
        n > 0;
        n--) {
        if(connected) furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        in->drag_lock = false;
    }

    for(uint8_t n = bm_take_requests(&in->toggle_scroll, &bm_mouse->req_done.toggle_scroll);
        n > 0;
        n--) {
        bm_mouse->scroll_mode = !bm_mouse->scroll_mode;
    }
}

static int32_t bm_mouse_thread(void* context) {
    BmMouse* bm_mouse = context;
    BmInput* in = &bm_mouse->input;

    uint8_t last_drawn_pct = 255;
    uint32_t last_bar_tick = 0;

    uint32_t next_deadline = furi_get_tick();
    uint32_t last_tick = next_deadline;

    while(true) {
        const BmSettings* settings = bm_mouse->settings;

        uint16_t tick_hz = settings->tick_hz;
        if(tick_hz < 10) tick_hz = 10;
        uint32_t tick_ms = 1000u / tick_hz;
        if(tick_ms < 1) tick_ms = 1;

        next_deadline += tick_ms;
        uint32_t now = furi_get_tick();
        int32_t remaining = (int32_t)(next_deadline - now);

        if(remaining < 0 || remaining > (int32_t)(tick_ms * 4)) {
            next_deadline = now + tick_ms;
            remaining = (int32_t)tick_ms;
        }

        uint32_t flags =
            furi_thread_flags_wait(BM_FLAG_STOP, FuriFlagWaitAny, (uint32_t)remaining);
        if((flags & BM_FLAG_STOP) != 0) break;

        now = furi_get_tick();
        uint32_t elapsed_ms = now - last_tick;
        last_tick = now;
        if(elapsed_ms == 0) elapsed_ms = 1;
        if(elapsed_ms > 200) elapsed_ms = 200; /* do not teleport after a stall */
        float dt = (float)elapsed_ms / 1000.0f;

        bool connected = furi_hal_hid_is_connected();
        bm_mouse_service_requests(bm_mouse, connected);

        int8_t dir_x = (int8_t)((in->right ? 1 : 0) - (in->left ? 1 : 0));
        int8_t dir_y = 0;
        int8_t scroll_dir = 0;

        if(bm_mouse->scroll_mode) {
            scroll_dir = (int8_t)((in->up ? 1 : 0) - (in->down ? 1 : 0));
        } else {
            dir_y = (int8_t)((in->down ? 1 : 0) - (in->up ? 1 : 0));
        }

        if(settings->invert_y) dir_y = (int8_t)-dir_y;
        if(settings->invert_scroll) scroll_dir = (int8_t)-scroll_dir;

        bool moving = (dir_x != 0 || dir_y != 0);

        bool quit = false;
        if(in->back_held && !in->back_consumed) {
            bm_mouse->back_hold_ms += elapsed_ms;
            if(bm_mouse->back_hold_ms >= BM_EXIT_HOLD && !bm_mouse->exit_requested) {
                bm_mouse->exit_requested = true;
                in->back_consumed = true; /* so releasing Back is not a click */
                quit = true;
            }
        } else if(!in->back_held) {
            bm_mouse->back_hold_ms = 0;
        }

        bool exit_warn = in->back_held && !in->back_consumed &&
                         (bm_mouse->back_hold_ms >= BM_EXIT_HOLD / 4);

        float max_speed = (float)settings->max_speed;
        float min_speed = (float)settings->min_speed;
        if(moving) {
            bm_mouse->hold_ms += elapsed_ms;
        } else {
            bm_mouse->hold_ms = 0;
        }

        float ramp = 1.0f;
        if(settings->accel_ms > 0) {
            ramp = (float)bm_mouse->hold_ms / (float)settings->accel_ms;
            if(ramp > 1.0f) ramp = 1.0f;
            ramp = ramp * ramp; /* gentle at first, quick once moving */
        }

        float speed = min_speed + (max_speed - min_speed) * ramp;
        if(speed < 0.0f) speed = 0.0f;

        float target_vx = (float)dir_x * speed;
        float target_vy = (float)dir_y * speed;

        if(dir_x != 0 && dir_y != 0) {
            target_vx *= BM_DIAG;
            target_vy *= BM_DIAG;
        }

        bool gliding = false;

        if(settings->ice) {
            float accel_ms = (float)settings->accel_ms;
            if(accel_ms < 1.0f) accel_ms = 1.0f;
            float max_delta = max_speed * (dt * 1000.0f / accel_ms);

            float ddx = target_vx - bm_mouse->vx;
            float ddy = target_vy - bm_mouse->vy;

            if(moving) {
                if(ddx > max_delta) ddx = max_delta;
                if(ddx < -max_delta) ddx = -max_delta;
                if(ddy > max_delta) ddy = max_delta;
                if(ddy < -max_delta) ddy = -max_delta;
                bm_mouse->vx += ddx;
                bm_mouse->vy += ddy;
            } else {
                float glide_ms = (float)settings->ice_glide_ms;
                if(glide_ms < 1.0f) glide_ms = 1.0f;
                float decay = powf(0.5f, (dt * 1000.0f) / glide_ms);
                bm_mouse->vx *= decay;
                bm_mouse->vy *= decay;

                if(fabsf(bm_mouse->vx) < BM_ICE_STOP) bm_mouse->vx = 0.0f;
                if(fabsf(bm_mouse->vy) < BM_ICE_STOP) bm_mouse->vy = 0.0f;
                gliding = (bm_mouse->vx != 0.0f || bm_mouse->vy != 0.0f);
            }
        } else {
            bm_mouse->vx = target_vx;
            bm_mouse->vy = target_vy;

            if(!moving) {
                bm_mouse->acc_x = 0.0f;
                bm_mouse->acc_y = 0.0f;
            }
        }

        /* PORT: encoder impulses (see bm_mouse_input_event_encoder). Added after the idle
           accumulator reset above, which would otherwise discard them. */
        if(bm_mouse->is_encoder) {
            int32_t ex = in->enc_x_units;
            int32_t ey = in->enc_y_units;
            int32_t ux = ex - bm_mouse->enc_x_done;
            int32_t uy = ey - bm_mouse->enc_y_done;
            bm_mouse->enc_x_done = ex;
            bm_mouse->enc_y_done = ey;

            bm_mouse->acc_x += (float)ux * BM_ENC_STEP_PX;
            if(bm_mouse->scroll_mode) {
                /* One notch per detent unit; wheel up (negative units) scrolls up. */
                int32_t notches = settings->invert_scroll ? uy : -uy;
                if(notches > BM_REPORT_MAX) notches = BM_REPORT_MAX;
                if(notches < -BM_REPORT_MAX) notches = -BM_REPORT_MAX;
                if(connected && notches != 0) furi_hal_hid_mouse_scroll((int8_t)notches);
            } else {
                float py = (float)uy * BM_ENC_STEP_PX;
                bm_mouse->acc_y += settings->invert_y ? -py : py;
            }
        }

        bm_mouse->acc_x += bm_mouse->vx * dt;
        bm_mouse->acc_y += bm_mouse->vy * dt;

        int8_t dx = bm_take_whole(&bm_mouse->acc_x);
        int8_t dy = bm_take_whole(&bm_mouse->acc_y);

        if(connected && (dx != 0 || dy != 0)) furi_hal_hid_mouse_move(dx, dy);

        if(scroll_dir != 0) {
            float scroll_speed = (float)settings->scroll_speed;
            bm_mouse->acc_scroll += (float)scroll_dir * scroll_speed * dt;
            int8_t notches = bm_take_whole(&bm_mouse->acc_scroll);
            if(connected && notches != 0) furi_hal_hid_mouse_scroll(notches);
        } else {
            bm_mouse->acc_scroll = 0.0f;
        }

        float shown = sqrtf(bm_mouse->vx * bm_mouse->vx + bm_mouse->vy * bm_mouse->vy);
        uint8_t speed_pct = 0;
        if(max_speed > 0.0f) {
            float pct = (shown * 100.0f) / max_speed;
            if(pct > 100.0f) pct = 100.0f;
            speed_pct = (uint8_t)(((uint8_t)pct / 20u) * 20u);
        }

        bool bar_due = (speed_pct != last_drawn_pct) &&
                       ((uint32_t)(now - last_bar_tick) >= BM_BAR_MIN_MS || speed_pct == 0 ||
                        speed_pct == 100);
        if(bar_due) last_bar_tick = now;

        bool redraw = false;
        with_view_model(
            bm_mouse->view,
            BmMouseModel * model,
            {
                bool up = in->up;
                bool down = in->down;
                bool left = in->left;
                bool right = in->right;
                bool back_down = in->back_held;
                bool drag = in->drag_lock;

                if(model->up != up || model->down != down || model->left != left ||
                   model->right != right || model->back_down != back_down ||
                   model->drag_lock != drag || model->btn_left != (in->ok_held || drag) ||
                   model->btn_right != back_down || model->scroll_mode != bm_mouse->scroll_mode ||
                   model->connected != connected || model->gliding != gliding ||
                   model->exit_warn != exit_warn || model->ice != (bool)settings->ice ||
                   bar_due) {
                    model->up = up;
                    model->down = down;
                    model->left = left;
                    model->right = right;
                    model->back_down = back_down;
                    model->drag_lock = drag;
                    model->btn_left = (in->ok_held || drag);
                    model->btn_right = back_down;
                    model->scroll_mode = bm_mouse->scroll_mode;
                    model->connected = connected;
                    model->gliding = gliding;
                    model->exit_warn = exit_warn;
                    model->ice = settings->ice;
                    model->speed_pct = speed_pct;
                    last_drawn_pct = speed_pct;
                    redraw = true;
                }
            },
            redraw);

        if(quit && bm_mouse->exit_callback) bm_mouse->exit_callback(bm_mouse->exit_context);
    }

    return 0;
}

/* --------------------------------------------------------------------- view */

static bool bm_mouse_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    /* State is taken straight from the input service; this only stops the view
       dispatcher from treating Back as "go back". */
    return true;
}

static void bm_mouse_enter_callback(void* context) {
    BmMouse* bm_mouse = context;

    memset(&bm_mouse->input, 0, sizeof(BmInput));
    bm_mouse->vx = 0.0f;
    bm_mouse->vy = 0.0f;
    bm_mouse->acc_x = 0.0f;
    bm_mouse->acc_y = 0.0f;
    bm_mouse->acc_scroll = 0.0f;
    bm_mouse->hold_ms = 0;
    bm_mouse->back_hold_ms = 0;
    bm_mouse->scroll_mode = false;
    bm_mouse->exit_requested = false;
    bm_mouse->enc_x_done = 0; /* PORT: matches the zeroed BmInput totals */
    bm_mouse->enc_y_done = 0;
    memset(&bm_mouse->req_done, 0, sizeof(bm_mouse->req_done)); /* PORT: same */

    with_view_model(
        bm_mouse->view,
        BmMouseModel * model,
        {
            memset(model, 0, sizeof(BmMouseModel));
            model->connected = furi_hal_hid_is_connected();
            model->ice = bm_mouse->settings->ice;
        },
        true);

    bm_mouse->input_subscription =
        furi_pubsub_subscribe(bm_mouse->input_events, bm_mouse_input_event, bm_mouse);

    bm_mouse->thread = furi_thread_alloc_ex("BmMouseMotion", 1024, bm_mouse_thread, bm_mouse);
    furi_thread_set_priority(bm_mouse->thread, FuriThreadPriorityHigh);
    furi_thread_start(bm_mouse->thread);
}

static void bm_mouse_exit_callback(void* context) {
    BmMouse* bm_mouse = context;

    if(bm_mouse->input_subscription) {
        furi_pubsub_unsubscribe(bm_mouse->input_events, bm_mouse->input_subscription);
        bm_mouse->input_subscription = NULL;
    }

    if(bm_mouse->thread) {
        furi_thread_flags_set(furi_thread_get_id(bm_mouse->thread), BM_FLAG_STOP);
        furi_thread_join(bm_mouse->thread);
        furi_thread_free(bm_mouse->thread);
        bm_mouse->thread = NULL;
    }

    /* Never leave a button stuck down on the host. */
    if(furi_hal_hid_is_connected()) {
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_LEFT);
        furi_hal_hid_mouse_release(HID_MOUSE_BTN_RIGHT);
    }
}

BmMouse* bm_mouse_alloc(const BmSettings* settings, NotificationApp* notifications) {
    BmMouse* bm_mouse = malloc(sizeof(BmMouse));
    memset(bm_mouse, 0, sizeof(BmMouse));

    bm_mouse->settings = settings;
    bm_mouse->notifications = notifications;
    bm_mouse->is_encoder =
        furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder; // PORT
    bm_mouse->input_events = furi_record_open(RECORD_INPUT_EVENTS);

    bm_mouse->view = view_alloc();
    view_set_context(bm_mouse->view, bm_mouse);
    view_allocate_model(bm_mouse->view, ViewModelTypeLockFree, sizeof(BmMouseModel));
    view_set_draw_callback(bm_mouse->view, bm_mouse_draw_callback);
    view_set_input_callback(bm_mouse->view, bm_mouse_input_callback);
    view_set_enter_callback(bm_mouse->view, bm_mouse_enter_callback);
    view_set_exit_callback(bm_mouse->view, bm_mouse_exit_callback);

    return bm_mouse;
}

void bm_mouse_free(BmMouse* bm_mouse) {
    furi_assert(bm_mouse);
    furi_record_close(RECORD_INPUT_EVENTS);
    view_free(bm_mouse->view);
    free(bm_mouse);
}

View* bm_mouse_get_view(BmMouse* bm_mouse) {
    furi_assert(bm_mouse);
    return bm_mouse->view;
}

void bm_mouse_set_exit_callback(BmMouse* bm_mouse, BmMouseExitCallback callback, void* context) {
    furi_assert(bm_mouse);
    bm_mouse->exit_callback = callback;
    bm_mouse->exit_context = context;
}

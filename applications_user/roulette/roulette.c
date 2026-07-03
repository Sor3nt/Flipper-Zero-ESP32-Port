/*
 * roulette.c — American roulette for T-Embed
 * Place bets, spin the wheel, win or lose money.
 * Controls: turn=nav/adjust, click=select, hold=MAX bet, side btn=back
 */

#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <notification/notification_messages_notes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* wheel + screen stuff */
#define N_POCKETS      38
#define SPIN_FRAMES    70
#define FRAME_MS       35
#define MIN_REVS       3
#define MAX_EXTRA_REVS 3

#define CX  30
#define CY  36
#define R_OUT 26
#define R_IN  17
#define R_HUB 8

/* money */
#define START_CASH  1000
#define MIN_BET     10
#define BROKE_GIVE  500

/* what screen are we on */
enum { S_BET, S_SPIN, S_RESULT, S_BROKE };

/* nav rows on the betting screen */
enum { NAV_TYPE, NAV_AMOUNT, NAV_SPIN, NAV_N };
/* nav sub-mode */
enum { NAV_BROWSE, NAV_EDIT_TYPE, NAV_EDIT_AMT };

/* bet types */
enum { B_RED, B_BLACK, B_GREEN, B_EVEN, B_ODD, B_LOW, B_HIGH, B_N };

typedef struct {
    int state;
    int nav_mode;
    int nav_row;

    float wheel_pos;
    int start_idx;
    int travel;
    int frame;
    int result;
    int bet_type;
    int bet;
    int cash;
    int payout;
    FuriMutex* mtx;
} Game;

typedef struct {
    int type; // 0=tick, 1=input
    InputEvent inp;
} Ev;

/* the actual wheel order, 37 means 00 */
static const int8_t wheel[N_POCKETS] = {
    0,28,9,26,30,11,7,20,32,17,5,22,34,15,3,24,36,13,1,37,
    27,10,25,29,12,8,19,31,18,6,21,33,16,4,23,35,14,2
};

static const uint8_t reds[] = {1,3,5,7,9,12,14,16,18,19,21,23,25,27,30,32,34,36};

static const char* bet_name[] = {"RED","BLACK","GREEN","EVEN","ODD","1-18","19-36"};
static const int bet_pay[] = {1,1,17,1,1,1,1};

/* sounds — only use what the firmware API actually exports */
/* fix: had to swap message_note_c7/a5/e6/g6 and message_delay_10/100  */
/*      because those aren't in the T-Embed firmware_api table.        */
/*      only c5, c6 and delay_25/50/250 are available.                  */
static const NotificationSequence snd_tick = {
    &message_note_c6, &message_delay_25, &message_sound_off, NULL
};
static const NotificationSequence snd_click = {
    &message_note_c5, &message_delay_25, &message_sound_off, NULL
};
static const NotificationSequence snd_win = {
    &message_note_c5, &message_delay_50, &message_note_c6, &message_delay_50,
    &message_note_c5, &message_delay_50, &message_note_c6, &message_delay_50,
    &message_sound_off, &message_vibro_on, &message_delay_50, &message_vibro_off, NULL
};
static const NotificationSequence snd_lose = {
    &message_note_c6, &message_delay_50, &message_note_c5, &message_delay_50,
    &message_sound_off, &message_vibro_on, &message_delay_25, &message_vibro_off, NULL
};
/* fix: green jackpot was using delay_100 which doesn't exist in the    */
/*      API table. replaced with delay_50 everywhere.                   */
static const NotificationSequence snd_jackpot = {
    &message_note_c6, &message_delay_50, &message_note_c5, &message_delay_50,
    &message_note_c6, &message_delay_50, &message_note_c5, &message_delay_50,
    &message_note_c6, &message_delay_50, &message_sound_off,
    &message_vibro_on, &message_delay_50, &message_vibro_off,
    &message_delay_50, &message_vibro_on, &message_delay_50, &message_vibro_off, NULL
};

static int is_red(int n) {
    for(unsigned i = 0; i < sizeof(reds); i++)
        if(reds[i] == n) return 1;
    return 0;
}

static int is_green(int n) { return n == 0 || n == 37; }

static int did_win(int bt, int num) {
    if(is_green(num)) return bt == B_GREEN;
    if(num < 1 || num > 36) return 0;
    switch(bt) {
        case B_RED:   return is_red(num);
        case B_BLACK: return !is_red(num);
        case B_EVEN:  return num % 2 == 0;
        case B_ODD:   return num % 2 == 1;
        case B_LOW:   return num <= 18;
        case B_HIGH:  return num >= 19;
    }
    return 0;
}

static void nstr(int n, char* b, int sz) {
    if(n == 37) snprintf(b, sz, "00");
    else snprintf(b, sz, "%d", n);
}

/* fix: roundf() isn't exported in the FAP API table so we do manual  */
/*      rounding with (int)(x + 0.5f) — works fine for positive vals  */
static int idx_at(float pos) {
    int i = ((int)(pos + 0.5f)) % N_POCKETS;
    if(i < 0) i += N_POCKETS;
    return i;
}

static const char* col_name(int n) {
    if(is_green(n)) return "GREEN";
    return is_red(n) ? "RED" : "BLACK";
}

/* added: clamp bet after any change so it never goes below MIN_BET  */
/*        or above current cash (prevents negative balance bug)     */
static void fix_bet(Game* g) {
    if(g->bet > g->cash) g->bet = g->cash;
    if(g->bet < MIN_BET) g->bet = MIN_BET;
}

/* ---- drawing ---- */

static void draw_wheel(Canvas* c, const Game* g) {
    for(int i = 0; i < N_POCKETS; i++) {
        float d = (float)i - g->wheel_pos;
        float a = -(float)M_PI/2.0f + d * (2.0f*(float)M_PI / N_POCKETS);
        float ca = cosf(a), sa = sinf(a);
        int x1 = CX + (int)(R_IN * ca), y1 = CY + (int)(R_IN * sa);
        int x2 = CX + (int)(R_OUT * ca), y2 = CY + (int)(R_OUT * sa);

        int n = wheel[i];
        canvas_set_color(c, ColorBlack);
        if(is_green(n)) {
            canvas_draw_line(c, x1, y1, x2, y2);
            canvas_draw_disc(c, x2, y2, 2);
        } else if(is_red(n)) {
            // dotted spoke for red pockets
            for(int t = 0; t <= 4; t++) {
                float f = (float)t/4.0f;
                canvas_draw_dot(c, x1+(int)((x2-x1)*f), y1+(int)((y2-y1)*f));
            }
        } else {
            canvas_draw_line(c, x1, y1, x2, y2);
        }
    }
    canvas_draw_circle(c, CX, CY, R_OUT);
    canvas_draw_circle(c, CX, CY, R_HUB);
    canvas_draw_disc(c, CX, CY, 2);

    // pointer triangle at top
    int pt = CY - R_OUT - 7, tip = CY - R_OUT - 1;
    if(pt < 0) pt = 0;
    canvas_draw_line(c, CX-3, pt, CX+3, pt);
    canvas_draw_line(c, CX-3, pt, CX, tip);
    canvas_draw_line(c, CX+3, pt, CX, tip);
}

static void draw_bet(Canvas* c, const Game* g) {
    char b[24];
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 9, AlignCenter, AlignBottom, "ROULETTE");

    canvas_set_font(c, FontSecondary);
    snprintf(b, sizeof(b), "Cash: $%d", g->cash);
    canvas_draw_str_aligned(c, 64, 19, AlignCenter, AlignBottom, b);
    canvas_draw_line(c, 6, 22, 122, 22);

    // bet type row
    int sel = (g->nav_row == NAV_TYPE);
    int edit = (sel && g->nav_mode == NAV_EDIT_TYPE);
    snprintf(b, sizeof(b), "%sType: %s%s%s",
        sel ? "> " : "  ", edit ? "<" : "", bet_name[g->bet_type], edit ? ">" : "");
    canvas_draw_str(c, 8, 32, b);

    // bet amount row
    sel = (g->nav_row == NAV_AMOUNT);
    edit = (sel && g->nav_mode == NAV_EDIT_AMT);
    snprintf(b, sizeof(b), "%sBet:  %s$%d%s",
        sel ? "> " : "  ", edit ? "<" : "", g->bet, edit ? ">" : "");
    canvas_draw_str(c, 8, 42, b);

    // payout
    snprintf(b, sizeof(b), "   pays %d:1", bet_pay[g->bet_type]);
    canvas_draw_str(c, 8, 50, b);

    // spin row
    sel = (g->nav_row == NAV_SPIN);
    canvas_set_font(c, FontPrimary);
    snprintf(b, sizeof(b), "%s[ SPIN ]", sel ? "> " : "  ");
    canvas_draw_str(c, 8, 61, b);
    canvas_draw_line(c, 0, 63, 128, 63);
}

static void draw_spin(Canvas* c, const Game* g) {
    char b[24];
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    draw_wheel(c, g);
    canvas_draw_line(c, 62, 0, 62, 57);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 66, 9, "SPINNING");

    int i = idx_at(g->wheel_pos);
    char nb[4];
    nstr(wheel[i], nb, sizeof(nb));
    canvas_set_font(c, FontBigNumbers);
    canvas_draw_str_aligned(c, 95, 32, AlignCenter, AlignBottom, nb);

    canvas_set_font(c, FontSecondary);
    snprintf(b, sizeof(b), "$%d on %s", g->bet, bet_name[g->bet_type]);
    canvas_draw_str_aligned(c, 95, 44, AlignCenter, AlignBottom, b);

    canvas_draw_line(c, 0, 58, 128, 58);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 63, "Spinning...");
}

static void draw_result(Canvas* c, const Game* g) {
    char b[24];
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);
    draw_wheel(c, g);
    canvas_draw_line(c, 62, 0, 62, 57);

    char nb[4];
    nstr(g->result, nb, sizeof(nb));
    canvas_set_font(c, FontBigNumbers);
    canvas_draw_str_aligned(c, 95, 26, AlignCenter, AlignBottom, nb);

    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 95, 35, AlignCenter, AlignBottom, col_name(g->result));

    canvas_set_font(c, FontPrimary);
    if(g->payout > 0) snprintf(b, sizeof(b), "WIN +$%d", g->payout);
    else snprintf(b, sizeof(b), "LOSE -$%d", -g->payout);
    canvas_draw_str_aligned(c, 95, 47, AlignCenter, AlignBottom, b);

    canvas_set_font(c, FontSecondary);
    snprintf(b, sizeof(b), "Cash: $%d", g->cash);
    canvas_draw_str_aligned(c, 95, 55, AlignCenter, AlignBottom, b);

    canvas_draw_line(c, 0, 58, 128, 58);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 63, "OK:Again  Btn:Menu");
}

static void draw_broke(Canvas* c) {
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 18, AlignCenter, AlignBottom, "BROKE!");
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 32, AlignCenter, AlignBottom, "You lost it all.");
    canvas_draw_str_aligned(c, 64, 44, AlignCenter, AlignBottom, "The house always wins.");
    canvas_draw_line(c, 8, 50, 120, 50);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 2, 63, "OK:Reset $500  Btn:Exit");
}

static void render(Canvas* c, void* ctx) {
    Game* g = ctx;
    switch(g->state) {
        case S_BET:    draw_bet(c, g); break;
        case S_SPIN:   draw_spin(c, g); break;
        case S_RESULT: draw_result(c, g); break;
        case S_BROKE:  draw_broke(c); break;
    }
}

static void on_input(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    Ev ev = {.type = 1, .inp = *e};
    furi_message_queue_put(q, &ev, FuriWaitForever);
}

static void on_tick(void* ctx) {
    FuriMessageQueue* q = ctx;
    Ev ev = {.type = 0};
    furi_message_queue_put(q, &ev, 0);
}

static void do_spin(Game* g) {
    g->cash -= g->bet;  // take the bet out before spinning
    g->start_idx = idx_at(g->wheel_pos);

    uint32_t r = furi_hal_random_get();
    int target = (int)(r % N_POCKETS);
    int extra = (int)(furi_hal_random_get() % MAX_EXTRA_REVS);
    int revs = MIN_REVS + extra;

    int fwd = target - g->start_idx;
    while(fwd < 0) fwd += N_POCKETS;

    g->travel = revs * N_POCKETS + fwd;
    g->frame = 0;
    g->state = S_SPIN;
}

static void step_spin(Game* g, NotificationApp* nf) {
    g->frame++;
    float t = (float)g->frame / (float)SPIN_FRAMES;
    if(t > 1) t = 1;
    float inv = 1 - t;
    float ease = 1 - inv*inv*inv; // cubic ease-out

    int prev = idx_at(g->wheel_pos);
    g->wheel_pos = (float)g->start_idx + ease * (float)g->travel;
    int now = idx_at(g->wheel_pos);

    if(g->frame >= SPIN_FRAMES) {
        g->wheel_pos = (float)(g->start_idx + g->travel);
        int fi = idx_at(g->wheel_pos);
        g->result = wheel[fi];
        g->state = S_RESULT;

        if(did_win(g->bet_type, g->result)) {
            int profit = g->bet * bet_pay[g->bet_type];
            g->cash += g->bet + profit;
            g->payout = profit;
            if(g->bet_type == B_GREEN) notification_message(nf, &snd_jackpot);
            else notification_message(nf, &snd_win);
        } else {
            g->payout = -g->bet;
            notification_message(nf, &snd_lose);
        }
    } else if(now != prev) {
        notification_message(nf, &snd_tick);
    }
}

/* fix: original version used InputKeyLeft/InputKeyRight for bet type    */
/*      but T-Embed doesn't have a dpad — those require holding the      */
/*      encoder button while turning which nobody's gonna figure out.    */
/*      redid the whole thing around turn=up/down + click=ok.            */
/*      added NAV_BROWSE/NAV_EDIT split so turn does different things    */
/*      depending on whether you're picking a row or editing a value.    */
int32_t roulette_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(Ev));

    Game* g = malloc(sizeof(Game));
    memset(g, 0, sizeof(Game));
    g->state = S_BET;
    g->nav_mode = NAV_BROWSE;
    g->nav_row = NAV_TYPE;
    g->bet_type = B_RED;
    g->bet = MIN_BET;
    g->cash = START_CASH;
    g->result = -1;
    g->mtx = furi_mutex_alloc(FuriMutexTypeNormal);

    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, render, g);
    view_port_input_callback_set(vp, on_input, q);

    FuriTimer* tm = furi_timer_alloc(on_tick, FuriTimerTypePeriodic, q);
    uint32_t tps = furi_kernel_get_tick_frequency();
    uint32_t pt = (tps * FRAME_MS) / 1000;
    if(!pt) pt = 1;
    furi_timer_start(tm, pt);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);
    NotificationApp* nf = furi_record_open(RECORD_NOTIFICATION);

    Ev ev;
    int running = 1;
    while(running) {
        FuriStatus s = furi_message_queue_get(q, &ev, 100);
        furi_mutex_acquire(g->mtx, FuriWaitForever);

        if(s != FuriStatusOk) goto done;

        if(ev.type == 0) {
            // timer tick
            if(g->state == S_SPIN) step_spin(g, nf);
            goto done;
        }

        // input event
        {
            InputKey k = ev.inp.key;
            InputType ty = ev.inp.type;

            // fix: encoder sends InputTypeShort for turn, InputTypeLong for hold
            //      so we check both to get click vs hold-to-max-bet behavior
            int up = (k == InputKeyUp && ty == InputTypeShort);
            int down = (k == InputKeyDown && ty == InputTypeShort);
            int ok = (k == InputKeyOk && ty == InputTypeShort);
            int ok_long = (k == InputKeyOk && ty == InputTypeLong);
            int back = (k == InputKeyBack && ty == InputTypeShort);

            if(g->state == S_BET) {
                if(g->nav_mode == NAV_BROWSE) {
                    if(up) { g->nav_row = (g->nav_row + NAV_N - 1) % NAV_N; notification_message(nf, &snd_click); }
                    else if(down) { g->nav_row = (g->nav_row + 1) % NAV_N; notification_message(nf, &snd_click); }
                    else if(ok) {
                        if(g->nav_row == NAV_TYPE) g->nav_mode = NAV_EDIT_TYPE;
                        else if(g->nav_row == NAV_AMOUNT) g->nav_mode = NAV_EDIT_AMT;
                        else if(g->nav_row == NAV_SPIN) {
                            fix_bet(g);
                            if(g->cash >= g->bet) do_spin(g);
                        }
                        notification_message(nf, &snd_click);
                    }
                    else if(back) running = 0;
                }
                else if(g->nav_mode == NAV_EDIT_TYPE) {
                    if(up) g->bet_type = (g->bet_type + B_N - 1) % B_N;
                    else if(down) g->bet_type = (g->bet_type + 1) % B_N;
                    else if(ok || back) { g->nav_mode = NAV_BROWSE; notification_message(nf, &snd_click); }
                }
                else if(g->nav_mode == NAV_EDIT_AMT) {
                    // fix: changed from +10/-10 steps to double/halve
                    //      10->20->40->80->160 is way faster than pressing 50 times
                    if(up) { g->bet *= 2; fix_bet(g); }       // double — quick ramp up
                    else if(down) { g->bet /= 2; fix_bet(g); }
                    // added: hold encoder = all-in (max bet in one move)
                    else if(ok_long) { g->bet = g->cash; fix_bet(g); notification_message(nf, &snd_win); }
                    else if(ok || back) { g->nav_mode = NAV_BROWSE; notification_message(nf, &snd_click); }
                }
            }
            else if(g->state == S_RESULT) {
                if(ok || back) {
                    if(g->cash < MIN_BET) g->state = S_BROKE;
                    else { fix_bet(g); g->state = S_BET; g->nav_mode = NAV_BROWSE; }
                }
            }
            else if(g->state == S_BROKE) {
                if(ok) { g->cash = BROKE_GIVE; g->bet = MIN_BET; g->state = S_BET; g->nav_mode = NAV_BROWSE; }
                else if(back) running = 0;
            }
        }

    done:
        furi_mutex_release(g->mtx);
        view_port_update(vp);
    }

    furi_timer_free(tm);
    view_port_enabled_set(vp, false);
    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(vp);
    furi_message_queue_free(q);
    furi_mutex_free(g->mtx);
    free(g);
    return 0;
}

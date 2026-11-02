/*
 * Host test for raw_edit_keys.h. Not part of the FAP (buildFap skips tests/).
 *
 *   zig cc -std=c11 -Wall -Wextra -o keys_host_test keys_host_test.c && ./keys_host_test
 *
 * It replays physical button scripts through an emulation of the paradoxt input
 * driver (button_poll() in targets/paradoxt/target_input.c: 20 ms poll, 2-poll
 * debounce, Repeat after 200 ms every 100 ms, one Short after Release for a
 * non-repeating click) and runs the events
 * through two consumers:
 *   - LEGACY: the upstream editor logic (Press/Repeat move, Short OK, Long OK menu)
 *   - NEW   : raw_edit_keys.h as used by run_editor_session()
 */
#define RAW_EDIT_KEYS_HOST_TEST 1
#include "../raw_edit_keys.h"

#include <stdio.h>
#include <string.h>

/* ---------------- driver emulation ---------------- */
#define POLL_MS            20
#define DEBOUNCE_POLLS     2
#define REPEAT_START_US    200000
#define REPEAT_RATE_US     100000
#define LONGPRESS_US       2500000

typedef struct {
    InputKey key;
    bool allow_repeat;
    bool level; /* scripted physical state */
    bool raw_pressed, debounced_pressed, wait_release, repeat_active;
    uint8_t debounce_polls;
    int64_t press_start_us, last_repeat_us;
} Btn;

typedef struct {
    InputEvent e;
    uint32_t cb_ms; /* when the GUI thread delivers it to the app's input callback */
} Ev;

static int64_t g_now_us;
static uint32_t g_seq;
static Ev g_ev[256];
static int g_nev;
static uint32_t g_cb_lag_ms; /* delivery delay of the Short event (GUI jitter) */

static void publish(InputKey key, InputType type, uint32_t seq, uint32_t lag) {
    InputEvent e;
    memset(&e, 0, sizeof(e));
    e.sequence_source = INPUT_SEQUENCE_SOURCE_HARDWARE;
    e.sequence_counter = seq;
    e.key = key;
    e.type = type;
    g_ev[g_nev].e = e;
    g_ev[g_nev].cb_ms = (uint32_t)(g_now_us / 1000) + lag;
    g_nev++;
}

static void btn_init(Btn* b, InputKey key, bool allow_repeat) {
    memset(b, 0, sizeof(*b));
    b->key = key;
    b->allow_repeat = allow_repeat;
    b->debounce_polls = DEBOUNCE_POLLS;
}

/* Mirror of button_poll() (paradoxt target_input.c). */
static void drv_poll(Btn* b) {
    bool raw = b->level;
    if(raw == b->raw_pressed) {
        if(b->debounce_polls < DEBOUNCE_POLLS) b->debounce_polls++;
    } else {
        b->raw_pressed = raw;
        b->debounce_polls = 1;
        if(raw && !b->debounced_pressed && b->key == InputKeyBack) b->press_start_us = g_now_us;
    }
    if(b->debounce_polls < DEBOUNCE_POLLS) return;

    if(b->wait_release) {
        if(!b->raw_pressed) {
            b->debounced_pressed = false;
            b->wait_release = false;
            b->press_start_us = 0;
            b->repeat_active = false;
        }
        return;
    }

    if(b->debounced_pressed == b->raw_pressed) {
        if(b->debounced_pressed && b->allow_repeat) {
            int64_t held = g_now_us - b->press_start_us;
            if(!b->repeat_active && held >= REPEAT_START_US) {
                b->repeat_active = true;
                b->last_repeat_us = g_now_us;
                publish(b->key, InputTypeRepeat, ++g_seq, 0);
            } else if(b->repeat_active && (g_now_us - b->last_repeat_us) >= REPEAT_RATE_US) {
                b->last_repeat_us = g_now_us;
                publish(b->key, InputTypeRepeat, ++g_seq, 0);
            }
        }
        return;
    }

    const bool pressed = b->raw_pressed;
    if(pressed && !b->press_start_us) b->press_start_us = g_now_us;
    b->debounced_pressed = pressed;
    if(pressed) {
        b->repeat_active = false;
        publish(b->key, InputTypePress, ++g_seq, 0);
        return;
    }
    int64_t dur = g_now_us - b->press_start_us;
    bool was_repeating = b->repeat_active;
    b->press_start_us = 0;
    b->repeat_active = false;
    publish(b->key, InputTypeRelease, ++g_seq, 0);
    if(!was_repeating && dur < LONGPRESS_US) {
        publish(b->key, InputTypeShort, ++g_seq, g_cb_lag_ms);
    }
}

/* ---------------- consumers ---------------- */
typedef enum { ModeEdit, ModeMenu } Mode;

typedef struct {
    int steps, zooms, toggles, menu_opens, exits, selects, nav;
    Mode mode;
} Out;

/* Upstream logic, as in the original run_editor_session(). */
static void legacy_event(Out* o, const InputEvent* e) {
    if(o->mode == ModeMenu) {
        if(e->type == InputTypeShort) {
            if(e->key == InputKeyDown || e->key == InputKeyUp) o->nav++;
            else if(e->key == InputKeyOk) { o->mode = ModeEdit; o->selects++; }
            else if(e->key == InputKeyBack) o->mode = ModeEdit;
        }
    } else if(e->type == InputTypePress || e->type == InputTypeRepeat) {
        if(e->key == InputKeyLeft || e->key == InputKeyRight) o->steps++;
        else if(e->key == InputKeyUp || e->key == InputKeyDown) o->zooms++;
    } else if(e->type == InputTypeShort) {
        if(e->key == InputKeyOk) o->toggles++;
        else if(e->key == InputKeyBack) o->exits++;
    } else if(e->type == InputTypeLong) {
        if(e->key == InputKeyOk) { o->mode = ModeMenu; o->menu_opens++; }
        else if(e->key == InputKeyBack) o->exits++;
    }
}

/* Mirror of the new run_editor_session() key handling. */
typedef struct {
    Out o;
    RawEditKey ok, back;
} NewState;

static void new_wake(NewState* s, const Ev* ev, uint32_t now_ms) {
    const bool got = ev != NULL;
    bool ok_click = false, back_click = false, step_rep = false;
    InputKey step_key = InputKeyMAX;

    if(s->o.mode == ModeEdit && raw_edit_key_hold_due(&s->ok, got ? ev->cb_ms : now_ms, RAW_EDIT_OK_HOLD_MS)) {
        s->o.mode = ModeMenu;
        s->o.menu_opens++;
    }
    if(got) {
        const InputEvent* e = &ev->e;
        if(e->key == InputKeyOk) {
            if(e->type == InputTypePress) raw_edit_key_press(&s->ok, ev->cb_ms);
            else if(e->type == InputTypeRelease) ok_click = raw_edit_key_release(&s->ok);
        } else if(e->key == InputKeyBack) {
            if(e->type == InputTypePress) raw_edit_key_press(&s->back, ev->cb_ms);
            else if(e->type == InputTypeRelease) back_click = raw_edit_key_release(&s->back);
        } else if(e->type == InputTypePress || e->type == InputTypeRepeat) {
            step_key = e->key;
            step_rep = e->type == InputTypeRepeat;
        }
    }
    if(s->o.mode == ModeMenu) {
        if((step_key == InputKeyDown || step_key == InputKeyUp) && !step_rep) s->o.nav++;
        if(ok_click) { s->o.mode = ModeEdit; s->o.selects++; }
        else if(back_click) s->o.mode = ModeEdit;
    } else {
        if(step_key == InputKeyLeft || step_key == InputKeyRight) s->o.steps++;
        else if(step_key == InputKeyUp || step_key == InputKeyDown) s->o.zooms++;
        if(ok_click) s->o.toggles++;
        if(back_click) s->o.exits++;
    }
}

/* ---------------- scripting ---------------- */
typedef struct {
    InputKey key;
    int press_ms, release_ms;
} Tap;

/* Replays taps; `pre` lets a scenario start in menu mode etc. Returns both outcomes. */
static void play(const Tap* taps, int ntaps, int total_ms, Out* legacy, NewState* neu) {
    Btn b[InputKeyMAX];
    for(int k = 0; k < InputKeyMAX; k++) btn_init(&b[k], (InputKey)k, k != InputKeyOk && k != InputKeyBack);
    g_seq = 0;
    g_nev = 0;
    memset(legacy, 0, sizeof(*legacy));
    memset(neu, 0, sizeof(*neu));

    int consumed = 0;
    for(int t = 0; t <= total_ms; t++) {
        g_now_us = (int64_t)t * 1000;
        for(int i = 0; i < ntaps; i++)
            if(taps[i].key < InputKeyMAX) {
                if(t == taps[i].press_ms) b[taps[i].key].level = true;
                if(t == taps[i].release_ms) b[taps[i].key].level = false;
            }
        if(t % POLL_MS == 0)
            for(int k = 0; k < InputKeyMAX; k++) drv_poll(&b[k]);

        /* deliver everything whose callback time has come, in publish order */
        while(consumed < g_nev && g_ev[consumed].cb_ms <= (uint32_t)t) {
            const Ev* ev = &g_ev[consumed++];
            legacy_event(legacy, &ev->e);
            if(ev->e.type == InputTypePress || ev->e.type == InputTypeRelease ||
               ev->e.type == InputTypeRepeat)
                new_wake(neu, ev, (uint32_t)t);
        }
        new_wake(neu, NULL, (uint32_t)t); /* timeout wake-up: OK hold may come due */
    }
}

static int g_fail;
#define CHECK(cond, ...) \
    do { \
        if(!(cond)) { \
            g_fail++; \
            printf("    FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while(0)

static void report(const char* name, const Out* l, const NewState* n) {
    printf("%-34s legacy[step %d zoom %d tgl %d menu %d exit %d sel %d]  new[step %d zoom %d tgl %d menu %d exit %d sel %d nav %d]\n",
           name, l->steps, l->zooms, l->toggles, l->menu_opens, l->exits, l->selects,
           n->o.steps, n->o.zooms, n->o.toggles, n->o.menu_opens, n->o.exits, n->o.selects, n->o.nav);
}

int main(void) {
    Out l;
    NewState n;

    { /* arrow click: one step */
        Tap t[] = {{InputKeyLeft, 100, 180}};
        play(t, 1, 600, &l, &n);
        report("Left click 80ms", &l, &n);
        CHECK(g_nev == 3 &&
                  g_ev[0].e.type == InputTypePress &&
                  g_ev[1].e.type == InputTypeRelease &&
                  g_ev[2].e.type == InputTypeShort,
              "driver click stream should be Press, Release, Short; got %d events", g_nev);
        CHECK(l.steps == 1, "driver stream must produce one step, got %d", l.steps);
        CHECK(n.o.steps == 1, "editor must step once, got %d", n.o.steps);
    }
    { /* zoom click */
        Tap t[] = {{InputKeyUp, 100, 180}};
        play(t, 1, 600, &l, &n);
        report("Up click 80ms (zoom)", &l, &n);
        CHECK(l.zooms == 1 && n.o.zooms == 1, "zoom legacy %d new %d", l.zooms, n.o.zooms);
    }
    { /* arrow hold with repeats: identical, no Short after a repeat */
        Tap t[] = {{InputKeyRight, 100, 1100}};
        play(t, 1, 1600, &l, &n);
        report("Right hold 1000ms", &l, &n);
        CHECK(n.o.steps == l.steps && n.o.steps >= 8, "hold steps legacy %d new %d", l.steps, n.o.steps);
    }
    { /* OK click toggles A/B once */
        Tap t[] = {{InputKeyOk, 100, 180}};
        play(t, 1, 600, &l, &n);
        report("OK click 80ms", &l, &n);
        CHECK(n.o.toggles == 1 && n.o.menu_opens == 0, "new toggles %d menu %d", n.o.toggles, n.o.menu_opens);
    }
    { /* OK hold must open the menu without toggling on release */
        Tap t[] = {{InputKeyOk, 100, 800}};
        play(t, 1, 1500, &l, &n);
        report("OK hold 700ms", &l, &n);
        CHECK(l.menu_opens == 0, "legacy never gets Long on this driver (bug reproduction), got %d", l.menu_opens);
        CHECK(n.o.menu_opens == 1 && n.o.toggles == 0 && n.o.mode == ModeMenu,
              "new menu %d toggles %d mode %d", n.o.menu_opens, n.o.toggles, n.o.mode);
    }
    { /* hold past the driver's Short threshold */
        Tap t[] = {{InputKeyOk, 100, 3300}};
        play(t, 1, 4000, &l, &n);
        report("OK hold 3200ms (no trailing Short)", &l, &n);
        CHECK(n.o.menu_opens == 1 && n.o.toggles == 0, "new menu %d toggles %d", n.o.menu_opens, n.o.toggles);
    }
    { /* a quick re-press after a no-Short hold is not mistaken for an echo */
        Tap t[] = {{InputKeyOk, 100, 3300}, {InputKeyOk, 3321, 3400}};
        play(t, 2, 4000, &l, &n);
        report("long OK then immediate click", &l, &n);
        CHECK(n.o.menu_opens == 1 && n.o.selects == 1 && n.o.mode == ModeEdit,
              "menu %d selects %d mode %d", n.o.menu_opens, n.o.selects, n.o.mode);
    }
    { /* hold, then a click inside the open menu selects exactly once */
        Tap t[] = {{InputKeyOk, 100, 800}, {InputKeyOk, 1100, 1180}};
        play(t, 2, 2000, &l, &n);
        report("OK hold then OK click (menu)", &l, &n);
        CHECK(n.o.menu_opens == 1 && n.o.selects == 1 && n.o.mode == ModeEdit,
              "menu %d selects %d mode %d", n.o.menu_opens, n.o.selects, n.o.mode);
    }
    { /* two quick OK clicks remain two toggles */
        Tap t[] = {{InputKeyOk, 100, 180}, {InputKeyOk, 300, 380}};
        play(t, 2, 900, &l, &n);
        report("OK double click (120ms gap)", &l, &n);
        CHECK(n.o.toggles == 2, "toggles %d", n.o.toggles);
    }
    { /* hold just under / over the threshold, measured Press->Release */
        Tap under[] = {{InputKeyOk, 100, 460}}; /* driver sees ~340ms: click */
        play(under, 1, 1200, &l, &n);
        report("OK 360ms (click)", &l, &n);
        CHECK(n.o.toggles == 1 && n.o.menu_opens == 0, "under: toggles %d menu %d", n.o.toggles, n.o.menu_opens);
        Tap over[] = {{InputKeyOk, 100, 640}};
        play(over, 1, 1200, &l, &n);
        report("OK 540ms (hold)", &l, &n);
        CHECK(n.o.toggles == 0 && n.o.menu_opens == 1, "over: toggles %d menu %d", n.o.toggles, n.o.menu_opens);
    }
    { /* Back click / long hold both leave the editor once */
        Tap t[] = {{InputKeyBack, 100, 180}};
        play(t, 1, 600, &l, &n);
        report("Back click", &l, &n);
        CHECK(n.o.exits == 1, "back click exits %d", n.o.exits);
        Tap h[] = {{InputKeyBack, 100, 3300}};
        play(h, 1, 4000, &l, &n);
        report("Back hold 3200ms", &l, &n);
        CHECK(n.o.exits == 1, "back hold exits %d", n.o.exits);
    }
    { /* delayed Short delivery does not change the physical Press/Release count */
        g_cb_lag_ms = 10;
        Tap t[] = {{InputKeyLeft, 100, 180}, {InputKeyOk, 400, 480}};
        play(t, 2, 900, &l, &n);
        report("click with 10ms Short lag", &l, &n);
        CHECK(n.o.steps == 1 && n.o.toggles == 1, "steps %d toggles %d", n.o.steps, n.o.toggles);
        g_cb_lag_ms = 0;
    }
    { /* arrow hold, release, immediate re-tap: the re-tap is a real step */
        Tap t[] = {{InputKeyLeft, 100, 600}, {InputKeyLeft, 700, 780}};
        play(t, 2, 1200, &l, &n);
        report("Left hold then re-tap", &l, &n);
        CHECK(n.o.steps == l.steps, "steps legacy %d new %d", l.steps, n.o.steps);
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

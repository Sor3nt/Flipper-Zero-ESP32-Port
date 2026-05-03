/* wolf3d_input.cpp — Bridge zwischen Furi-Input-PubSub und SDL_Event-Queue.
 *
 * T-Embed hat: Side-Button (InputKeyBack), Encoder-Button (InputKeyOk),
 * Encoder-Drehung (InputKeyUp/Down/Left/Right via Short-Press-Pulses).
 * Wolf3D-Mapping (vorläufig — Stage 2 verfeinert):
 *
 *   Side short        → SPACE (open doors / activate)
 *   Side long         → ESCAPE (Menü)
 *   Encoder click     → CTRL (FIRE)
 *   Encoder long      → SHIFT (RUN, später UP-hold)
 *   Encoder ↺ / ↻     → LEFT / RIGHT (~130 ms hold)
 *
 * Scancodes: Wolf4SDL nutzt seine eigenen `sc_*` defines (id_in.h),
 * angelehnt an PC-AT-Set-1. Wir mappen direkt darauf.
 */

#include "SDL.h"
#include "wolf3d_glue.h"

#include <string.h>

#include <furi.h>
#include <input/input.h>

extern "C" int wolf3d_is_ingame(void);

/* Wolf4SDLs id_in.h mappt sc_*-Konstanten auf SDLK_*-Werte (SDL2-Keycodes,
 * nicht PC-AT-Scancodes). Wir nutzen die SDLK_*-Defines aus unserer Shim. */
namespace sc {
    constexpr int Escape    = SDLK_ESCAPE;
    constexpr int Return    = SDLK_RETURN;
    constexpr int Control   = SDLK_LCTRL;
    constexpr int LShift    = SDLK_LSHIFT;
    constexpr int Space     = SDLK_SPACE;
    constexpr int UpArrow   = SDLK_UP;
    constexpr int LeftArrow = SDLK_LEFT;
    constexpr int RightArrow= SDLK_RIGHT;
    constexpr int DownArrow = SDLK_DOWN;
    constexpr int Y         = SDLK_y;
    constexpr int N         = SDLK_n;
    constexpr int Tab       = SDLK_TAB;
    constexpr int F1        = SDLK_F1;
}

namespace {

constexpr int EVENT_QUEUE_SIZE = 32;
SDL_Event       s_queue[EVENT_QUEUE_SIZE];
volatile uint8_t s_q_wr = 0;
volatile uint8_t s_q_rd = 0;
FuriMutex*      s_q_mutex = nullptr;

FuriPubSubSubscription* s_input_sub = nullptr;
FuriPubSub*             s_input_events = nullptr;

FuriTimer* s_release_left = nullptr;
FuriTimer* s_release_right = nullptr;
FuriTimer* s_release_fire = nullptr;
FuriTimer* s_release_use = nullptr;

void push_key_event(uint32_t type, int scancode) {
    if(!s_q_mutex) return;
    if(furi_mutex_acquire(s_q_mutex, 50) != FuriStatusOk) return;
    uint8_t next = (s_q_wr + 1) % EVENT_QUEUE_SIZE;
    if(next != s_q_rd) {
        SDL_Event ev{};
        ev.type = type;
        ev.key.type = type;
        ev.key.state = (type == SDL_KEYDOWN) ? 1 : 0;
        ev.key.keysym.scancode = scancode;
        ev.key.keysym.sym = scancode;
        s_queue[s_q_wr] = ev;
        s_q_wr = next;
    }
    furi_mutex_release(s_q_mutex);
}

/* Release timer callbacks — wir wissen nicht welche Taste tatsächlich
 * unterwegs ist, weil eine Taste den Timer für die Drehrichtung benutzt
 * (s_release_left = ↺, s_release_right = ↻). Lösung: alle 4 Pfeil-Tasten
 * loslassen — SDL-Events sind idempotent, doppeltes KEYUP ist OK. */
void release_left_cb(void*)  {
    push_key_event(SDL_KEYUP, sc::UpArrow);
    push_key_event(SDL_KEYUP, sc::LeftArrow);
}
void release_right_cb(void*) {
    push_key_event(SDL_KEYUP, sc::DownArrow);
    push_key_event(SDL_KEYUP, sc::RightArrow);
}
void release_fire_cb(void*)  { push_key_event(SDL_KEYUP, sc::Control); }
void release_use_cb(void*)   { push_key_event(SDL_KEYUP, sc::Space); }

void input_callback(const void* value, void* /*ctx*/) {
    const InputEvent* e = static_cast<const InputEvent*>(value);

    if(e->key == InputKeyBack) {
        if(e->type == InputTypeShort) {
            /* Im Menu: Side-Short = ESC (zurück). Im Spiel: SPACE (USE/Door). */
            if(wolf3d_is_ingame()) {
                push_key_event(SDL_KEYDOWN, sc::Space);
                furi_timer_start(s_release_use, pdMS_TO_TICKS(120));
            } else {
                push_key_event(SDL_KEYDOWN, sc::Escape);
                push_key_event(SDL_KEYUP,   sc::Escape);
            }
        } else if(e->type == InputTypeLong) {
            push_key_event(SDL_KEYDOWN, sc::Escape);
            push_key_event(SDL_KEYUP,   sc::Escape);
        }
        return;
    }

    if(e->key == InputKeyOk) {
        if(e->type == InputTypeShort) {
            push_key_event(SDL_KEYDOWN, sc::Control);
            furi_timer_start(s_release_fire, pdMS_TO_TICKS(120));
        } else if(e->type == InputTypeLong) {
            /* Long press: walk forward (UP held). KEIN LSHIFT (Run) — der
             * Walk-Speed wurde via wl_def.h::BASEMOVE auf ~75% von Run
             * angepasst, damit's auf dem T-Embed nicht zu schnell läuft. */
            push_key_event(SDL_KEYDOWN, sc::UpArrow);
        } else if(e->type == InputTypeRelease) {
            push_key_event(SDL_KEYUP, sc::UpArrow);
        }
        return;
    }

    /* Encoder-Drehung:
     *   T-Embed liefert Up/Down (Drehen ohne Knopf) und Left/Right (mit
     *   Knopf gehalten). Routing kontextabhängig:
     *     im Spiel:  egal welche Eingabe → SDLK_LEFT/RIGHT (Player turn)
     *     im Menu:   egal welche Eingabe → SDLK_UP/DOWN (Navigation)
     *   ↺ (Up/Left) = "negativ", ↻ (Down/Right) = "positiv". */
    if(e->type == InputTypeShort &&
       (e->key == InputKeyUp || e->key == InputKeyDown ||
        e->key == InputKeyLeft || e->key == InputKeyRight)) {
        bool ccw = (e->key == InputKeyUp) || (e->key == InputKeyLeft);
        bool ingame = wolf3d_is_ingame();
        int sc_code;
        FuriTimer* t;
        if(ingame) {
            sc_code = ccw ? sc::LeftArrow : sc::RightArrow;
            t       = ccw ? s_release_left : s_release_right;
        } else {
            sc_code = ccw ? sc::UpArrow   : sc::DownArrow;
            t       = ccw ? s_release_left : s_release_right;
        }
        push_key_event(SDL_KEYDOWN, sc_code);
        furi_timer_start(t, pdMS_TO_TICKS(130));
    }
}

} /* namespace */

extern "C" int wolf3d_event_pop(SDL_Event* event) {
    if(!s_q_mutex || !event) return 0;
    int got = 0;
    if(furi_mutex_acquire(s_q_mutex, 0) == FuriStatusOk) {
        if(s_q_rd != s_q_wr) {
            *event = s_queue[s_q_rd];
            s_q_rd = (s_q_rd + 1) % EVENT_QUEUE_SIZE;
            got = 1;
        }
        furi_mutex_release(s_q_mutex);
    }
    return got;
}

extern "C" void wolf3d_input_init(FuriPubSub* input_events) {
    if(s_input_sub) return;
    s_input_events = input_events;
    s_q_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    s_release_left  = furi_timer_alloc(release_left_cb,  FuriTimerTypeOnce, nullptr);
    s_release_right = furi_timer_alloc(release_right_cb, FuriTimerTypeOnce, nullptr);
    s_release_fire  = furi_timer_alloc(release_fire_cb,  FuriTimerTypeOnce, nullptr);
    s_release_use   = furi_timer_alloc(release_use_cb,   FuriTimerTypeOnce, nullptr);
    s_input_sub = furi_pubsub_subscribe(input_events, input_callback, nullptr);
}

extern "C" void wolf3d_input_deinit(void) {
    if(s_input_sub && s_input_events) {
        furi_pubsub_unsubscribe(s_input_events, s_input_sub);
        s_input_sub = nullptr;
    }
    if(s_release_left)  { furi_timer_free(s_release_left);  s_release_left = nullptr; }
    if(s_release_right) { furi_timer_free(s_release_right); s_release_right = nullptr; }
    if(s_release_fire)  { furi_timer_free(s_release_fire);  s_release_fire = nullptr; }
    if(s_release_use)   { furi_timer_free(s_release_use);   s_release_use = nullptr; }
    if(s_q_mutex) { furi_mutex_free(s_q_mutex); s_q_mutex = nullptr; }
}

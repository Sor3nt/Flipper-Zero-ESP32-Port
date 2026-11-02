#pragma once

/*
 * Input normalizer for boards whose input driver does not behave like a stock Flipper
 * (BOARD_INPUT_SHORT_AFTER_RELEASE, e.g. paradoxt). Same contract as i2c_tools_cli's i2c_input.h.
 *
 * That driver reports a click as Press/Release followed by Short, and never sends
 * InputTypeLong for OK/Back (arrows send InputTypeRepeat while held). Upstream
 * acted on Press, so before the driver fix every click acted twice (mode skipped one entry, the
 * angle moved 20 degrees instead of 10).
 *
 * servo_input_translate() turns the raw stream back into stock semantics: one InputTypeShort per
 * click, InputTypeLong for an OK/Back hold, InputTypeRepeat passed through. On a stock board it
 * is a pass-through. The app only acts on Short / Long / Repeat.
 */

#include <input/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <boards/board.h>

#define SERVO_INPUT_LONG_MS 600 // OK/Back hold that counts as Long (driver's Short cutoff is 2.5 s)
typedef struct {
    bool down;
    uint32_t press_ms;
} ServoInputKey;

typedef struct {
    ServoInputKey key[InputKeyMAX];
    InputKey swallow_key;
    bool swallow_armed;
} ServoInput;

static inline void servo_input_reset(ServoInput* s) {
    for(size_t i = 0; i < InputKeyMAX; i++) {
        s->key[i].down = false;
        s->key[i].press_ms = 0;
    }
    s->swallow_armed = false;
    s->swallow_key = InputKeyMAX;
}

/** Feed one raw event.
 * @return true and fill `out` when the app should handle `out`; false to drop `in`. */
static inline bool
    servo_input_translate(ServoInput* s, const InputEvent* in, uint32_t now_ms, InputEvent* out) {
#if defined(BOARD_INPUT_SHORT_AFTER_RELEASE) && BOARD_INPUT_SHORT_AFTER_RELEASE
    if(in->key >= InputKeyMAX) return false;
    if(s->swallow_armed) {
        const bool trailing_short =
            in->type == InputTypeShort && in->key == s->swallow_key;
        s->swallow_armed = false;
        if(trailing_short) return false;
    }
    ServoInputKey* k = &s->key[in->key];
    const bool hold_key = (in->key == InputKeyOk || in->key == InputKeyBack);

    switch(in->type) {
    case InputTypePress:
        k->down = true;
        k->press_ms = now_ms;
        return false;

    case InputTypeRelease:
        if(k->down && hold_key && (now_ms - k->press_ms) >= SERVO_INPUT_LONG_MS) {
            k->down = false;
            *out = *in;
            out->type = InputTypeLong;
            // A Short may follow this release in the same driver poll.
            s->swallow_armed = true;
            s->swallow_key = in->key;
            return true;
        }
        k->down = false;
        return false; // clicks act on Short, never on Release

    case InputTypeShort:
        *out = *in;
        return true;

    case InputTypeLong:
    case InputTypeRepeat:
        *out = *in;
        return true;

    default:
        return false;
    }
#else
    (void)s;
    (void)now_ms;
    switch(in->type) {
    case InputTypeShort:
    case InputTypeLong:
    case InputTypeRepeat:
        *out = *in;
        return true;
    default:
        return false;
    }
#endif
}

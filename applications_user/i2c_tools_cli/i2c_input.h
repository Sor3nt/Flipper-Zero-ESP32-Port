#pragma once

/*
 * Input normalizer for boards whose input driver does not behave like a stock Flipper
 * (BOARD_INPUT_SHORT_AFTER_RELEASE, e.g. paradoxt).
 *
 * That driver reports a click as Press/Release followed by Short, and never sends
 * InputTypeLong for OK/Back (arrows send InputTypeRepeat while held). Before the driver fix,
 * upstream InputTypeRelease actions could fire twice per click, and its
 * "Long OK" / "Long Back" gestures (READ/WRITE and HEX/ASCII toggles) could never happen.
 *
 * i2c_input_translate() turns the raw stream back into stock semantics: one InputTypeShort per
 * click, InputTypeLong for an OK/Back hold, InputTypeRepeat passed through. On a stock board it
 * is a pass-through. The UI then only acts on Short / Long / Repeat.
 */

#include <input/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <boards/board.h>

#define I2C_INPUT_LONG_MS 600 // OK/Back hold that counts as Long (driver's Short cutoff is 2.5 s)
typedef struct {
    bool down;
    uint32_t press_ms;
} I2cInputKey;

typedef struct {
    I2cInputKey key[InputKeyMAX];
    InputKey swallow_key;
    bool swallow_armed;
} I2cInput;

static inline void i2c_input_reset(I2cInput* s) {
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
    i2c_input_translate(I2cInput* s, const InputEvent* in, uint32_t now_ms, InputEvent* out) {
#if defined(BOARD_INPUT_SHORT_AFTER_RELEASE) && BOARD_INPUT_SHORT_AFTER_RELEASE
    if(in->key >= InputKeyMAX) return false;
    if(s->swallow_armed) {
        const bool trailing_short =
            in->type == InputTypeShort && in->key == s->swallow_key;
        s->swallow_armed = false;
        if(trailing_short) return false;
    }
    I2cInputKey* k = &s->key[in->key];
    const bool hold_key = (in->key == InputKeyOk || in->key == InputKeyBack);

    switch(in->type) {
    case InputTypePress:
        k->down = true;
        k->press_ms = now_ms;
        return false;

    case InputTypeRelease:
        if(k->down && hold_key && (now_ms - k->press_ms) >= I2C_INPUT_LONG_MS) {
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

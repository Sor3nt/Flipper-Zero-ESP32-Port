#pragma once

/*
 * Canonical (stock Flipper) input stream for boards whose driver is not stock.
 *
 * Header-only helper for apps and FAPs written for the STM32 Flipper.
 *
 * With BOARD_INPUT_SHORT_AFTER_RELEASE (paradoxt, targets/paradoxt/target_input.c) the
 * driver reports
 *   - a click as Press, Release and then, in the same poll, a Short event
 *     carrying the next sequence number,
 *   - a held arrow as Press, Repeat..., Release (no trailing Short),
 *   - and it never sends InputTypeLong for OK/Back (a 2.5 s Back hold opens Power
 *     from Home instead).
 * Before the Paradoxt driver fix, code acting on Press/Release could fire twice per
 * click; the missing OK/Back Long events still require normalization.
 *
 * input_canonical_feed() turns that stream back into what a stock Flipper sends:
 *   tap            Press, Short, Release
 *   arrow held     Press, Repeat..., Release
 *   OK/Back held   Press, Long, Release          (Long at release, >= LONG_MS held)
 * The driver's trailing Short is dropped because the release already generated
 * its canonical Short or Long. One physical action therefore yields exactly one
 * logical event.
 *
 * On boards without BOARD_INPUT_SHORT_AFTER_RELEASE the event is passed through.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef INPUT_CANONICAL_HOST_TEST
/* Host unit test: mirror of components/input/input.h and the board flag. */
#define BOARD_INPUT_SHORT_AFTER_RELEASE INPUT_CANONICAL_HOST_TEST
#define INPUT_SEQUENCE_SOURCE_HARDWARE (0u)
typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack,
    InputKeyMAX,
} InputKey;
typedef enum {
    InputTypePress,
    InputTypeRelease,
    InputTypeShort,
    InputTypeLong,
    InputTypeRepeat,
    InputTypeMAX,
} InputType;
typedef struct {
    union {
        uint32_t sequence;
        struct {
            uint8_t sequence_source : 2;
            uint32_t sequence_counter : 30;
        };
    };
    InputKey key;
    InputType type;
} InputEvent;
#else
#include <input/input.h>
#include <boards/board.h>
#endif

/** OK/Back held at least this long (ms) is a Long. */
#define INPUT_CANONICAL_LONG_MS 600U
/** Upper bound of events produced by one input_canonical_feed() call. */
#define INPUT_CANONICAL_MAX_OUT 2U

typedef struct {
    bool down;
    bool repeated; /* auto-repeat since the last Press: the driver sends no trailing Short */
    uint32_t press_ms;
} InputCanonicalKey;

typedef struct {
    InputCanonicalKey key[InputKeyMAX];
} InputCanonical;

static inline void input_canonical_reset(InputCanonical* s) {
    for(size_t i = 0; i < InputKeyMAX; i++) {
        s->key[i].down = false;
        s->key[i].repeated = false;
        s->key[i].press_ms = 0;
    }
}

/** Feed EVERY hardware input event, in order.
 *  @param now_ms  monotonic milliseconds at the moment the event is handled
 *  @param out     room for INPUT_CANONICAL_MAX_OUT events
 *  @return number of canonical events written to `out` (0, 1 or 2) */
static inline size_t input_canonical_feed(
    InputCanonical* s,
    const InputEvent* in,
    uint32_t now_ms,
    InputEvent out[INPUT_CANONICAL_MAX_OUT]) {
#if defined(BOARD_INPUT_SHORT_AFTER_RELEASE) && BOARD_INPUT_SHORT_AFTER_RELEASE
    if(in->sequence_source != INPUT_SEQUENCE_SOURCE_HARDWARE || in->key >= InputKeyMAX) {
        out[0] = *in; /* not from the board driver: not ours to rewrite */
        return 1;
    }

    InputCanonicalKey* k = &s->key[in->key];
    const bool hold_key = (in->key == InputKeyOk || in->key == InputKeyBack);

    switch(in->type) {
    case InputTypePress:
        k->down = true;
        k->repeated = false;
        k->press_ms = now_ms;
        out[0] = *in;
        return 1;

    case InputTypeRepeat:
        if(!k->down) return 0; /* repeat of a press we never saw */
        k->repeated = true;
        out[0] = *in;
        return 1;

    case InputTypeRelease: {
        const bool was_down = k->down;
        const bool was_repeated = k->repeated;
        const uint32_t held = (uint32_t)(now_ms - k->press_ms);
        k->down = false;
        k->repeated = false;
        if(!was_down) return 0; /* release of a press from before we started */

        size_t n = 0;
        if(!was_repeated) {
            out[n] = *in;
            out[n].type = (hold_key && held >= INPUT_CANONICAL_LONG_MS) ? InputTypeLong :
                                                                          InputTypeShort;
            n++;
        }
        out[n++] = *in;
        return n;
    }

    case InputTypeShort:
    case InputTypeLong:
    default:
        return 0; /* Short/Long are synthesized from press/release duration. */
    }
#else
    (void)s;
    (void)now_ms;
    out[0] = *in;
    return 1;
#endif
}

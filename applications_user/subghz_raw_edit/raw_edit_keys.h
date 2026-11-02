#pragma once
/*
 * Button handling for the Sub-GHz RAW Edit editor on this ESP32-S3 Flipper port.
 * (Port-specific addition, not part of upstream SubGHz-RAW-Edit.)
 *
 * The paradoxt input driver (targets/paradoxt/target_input.c) publishes physical
 * Press/Release and one Short event after Release. Acting on Press therefore
 * fires once per click.
 *   - OK and Back never emit InputTypeLong. The upstream "hold OK" gesture that
 *     opens the Save / Cut / Undo menu would be unreachable.
 *   - A key held long enough to Repeat gets no Short event; OK / Back held for
 *     BOARD_LONGPRESS_SHUTDOWN_MS get no Short event either.
 *
 * The editor works from Press / Release / Repeat events and measures click vs.
 * hold from Press to Release. Short/Long events are ignored.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef RAW_EDIT_KEYS_HOST_TEST
/* Host unit test: mirror of components/input/input.h. */
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
#endif

/** OK held at least this long opens the action menu (STM32 Flipper long press is ~400 ms). */
#define RAW_EDIT_OK_HOLD_MS 400U
typedef struct {
    bool down;
    bool fired; /* the hold already acted: its Release is not a click */
    uint32_t t0;
} RawEditKey;

static inline void raw_edit_key_press(RawEditKey* k, uint32_t now_ms) {
    k->down = true;
    k->fired = false;
    k->t0 = now_ms;
}

/** @return true if this Release completes a click (key was down and no hold fired) */
static inline bool raw_edit_key_release(RawEditKey* k) {
    const bool click = k->down && !k->fired;
    k->down = false;
    k->fired = false;
    return click;
}

/** Call whenever the hold may be acting (every wake-up).
 * @return true exactly once per press, when the key has been down for hold_ms */
static inline bool raw_edit_key_hold_due(RawEditKey* k, uint32_t now_ms, uint32_t hold_ms) {
    if(k->down && !k->fired && (uint32_t)(now_ms - k->t0) >= hold_ms) {
        k->fired = true;
        return true;
    }
    return false;
}

/** @return ms until the hold is due, or UINT32_MAX if none is pending */
static inline uint32_t raw_edit_key_hold_wait(const RawEditKey* k, uint32_t now_ms, uint32_t hold_ms) {
    if(!k->down || k->fired) return UINT32_MAX;
    const uint32_t held = (uint32_t)(now_ms - k->t0);
    return held >= hold_ms ? 0 : hold_ms - held;
}

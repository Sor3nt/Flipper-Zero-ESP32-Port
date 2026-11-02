#pragma once

#include <furi.h>
#include <input/input.h>
#include <boards/board.h>

/* Legacy filter for boards that explicitly define BOARD_INPUT_SYNTHETIC_CLICK.
 * Paradoxt no longer defines that flag: its driver now emits one physical
 * Press/Release pair, so hold-to-transmit handlers do not need an echo filter.
 *
 * Keep this compatibility helper for any other legacy driver that still emits
 * the duplicated click sequence. */
#define SUBGHZ_CLICK_ECHO_WINDOW_MS 20
#define SUBGHZ_CLICK_ECHO_SEQ_MASK  0x3FFFFFFFUL // InputEvent.sequence_counter is 30 bits

typedef struct {
    bool armed;
    bool swallow_release; // subghz_click_echo_filter_type(): echo Press seen
    InputKey key;
    uint32_t sequence;
    uint32_t armed_at;
} SubGhzClickEcho;

/** Feed every input event of the keys that drive TX.
 * @return true if `event` is the synthetic echo of the previous click (ignore it) */
static inline bool subghz_click_echo_filter(SubGhzClickEcho* echo, const InputEvent* event) {
#if defined(BOARD_INPUT_SYNTHETIC_CLICK) && BOARD_INPUT_SYNTHETIC_CLICK
    if(echo->armed && event->key == echo->key) {
        if(event->sequence_counter == echo->sequence &&
           (furi_get_tick() - echo->armed_at) <= furi_ms_to_ticks(SUBGHZ_CLICK_ECHO_WINDOW_MS)) {
            if(event->type == InputTypeRelease) echo->armed = false; // echo fully consumed
            return true;
        }
        echo->armed = false;
    }
    if(event->type == InputTypeRelease) {
        echo->armed = true;
        echo->key = event->key;
        echo->sequence = (event->sequence_counter + 1UL) & SUBGHZ_CLICK_ECHO_SEQ_MASK;
        echo->armed_at = furi_get_tick();
    }
#else
    UNUSED(echo);
    UNUSED(event);
#endif
    return false;
}

/** Same filter for callbacks that only see the event type of one button (widget
 * buttons): the echo is a Press arriving right after that button's Release. */
static inline bool subghz_click_echo_filter_type(SubGhzClickEcho* echo, InputType type) {
#if defined(BOARD_INPUT_SYNTHETIC_CLICK) && BOARD_INPUT_SYNTHETIC_CLICK
    if(type == InputTypePress && echo->armed) {
        echo->armed = false;
        if((furi_get_tick() - echo->armed_at) <= furi_ms_to_ticks(SUBGHZ_CLICK_ECHO_WINDOW_MS)) {
            echo->swallow_release = true; // the echo's Release follows
            return true;
        }
    } else if(type == InputTypeRelease) {
        if(echo->swallow_release) {
            echo->swallow_release = false;
            return true;
        }
        echo->armed = true;
        echo->armed_at = furi_get_tick();
    }
#else
    UNUSED(echo);
    UNUSED(type);
#endif
    return false;
}

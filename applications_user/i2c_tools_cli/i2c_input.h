#pragma once

/*
 * Input filter. Every board driver of this firmware sends the stock Flipper stream (a tap is
 * Press, Short, Release; a hold is Press, Long, Repeat..., Release), so the UI only has to act
 * on Short / Long / Repeat and ignore Press / Release. This used to rebuild the stock stream
 * from a driver that did not send it (and so depended on a board macro of the build); that
 * driver was fixed in the firmware.
 */

#include <input/input.h>
#include <furi_hal_board_info.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Encoder board (T-Embed, ThorChimera): see i2ctools_encoder_adjust() in i2c_tools_cli.c. */
    bool encoder;
    bool mod_seen; /* a "hold OK + turn" (Left/Right) happened at mod_ms */
    uint32_t mod_ms;
    bool step_seen; /* last wheel detent (Up/Down) at step_ms */
    uint32_t step_ms;
} I2cInput;

static inline void i2c_input_reset(I2cInput* s) {
    s->encoder = furi_hal_board_get_input_layout() == FuriHalBoardInputLayoutEncoder;
    s->mod_seen = false;
    s->mod_ms = 0;
    s->step_seen = false;
    s->step_ms = 0;
}

/** @return true and fill `out` when the app should handle the event; false to drop it. */
static inline bool
    i2c_input_translate(I2cInput* s, const InputEvent* in, uint32_t now_ms, InputEvent* out) {
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
}

#pragma once

#include "nrf24_mj_core.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Streaming DuckyScript subset interpreter for MouseJacker.
 *
 * Reads BadUSB-format script files from /ext/badusb (.txt) one line at a time
 * (no full-file buffering). Supports the subset:
 *   REM | // | #         — comments
 *   DEFAULTDELAY ms      — between-command delay
 *   DEFAULT_DELAY ms     — alias
 *   DELAY ms             — one-shot pause
 *   STRING text          — type ASCII text
 *   STRINGLN text        — STRING + ENTER
 *   ENTER | RETURN
 *   ESC  | ESCAPE
 *   TAB | SPACE | BACKSPACE | DELETE | INSERT
 *   HOME | END | PAGEUP | PAGEDOWN | CAPSLOCK
 *   UP | DOWN | LEFT | RIGHT (also -ARROW variants)
 *   F1..F12
 *   PRINTSCREEN | SCROLLLOCK | PAUSE
 *   <MOD> [<MOD> ...] [<KEY|char>]   — chord, e.g. "GUI r", "CTRL ALT DELETE"
 *
 * Unknown directives are skipped (last_warning is filled).
 * BadUSB-only commands (HOLD, RELEASE, ALTCHAR, etc.) are treated as unknown.
 */

struct Storage;

typedef struct {
    /* Public read-only state */
    char file_name[32]; /* basename, no path */
    size_t total_lines;
    size_t current_line;
    char last_warning[64]; /* "Skipped line N: <reason>" */

    /* Internal */
    struct Storage* storage;
    void* stream;
    void* line_buf;
    uint16_t default_delay_ms;
    bool eof;
    bool error;
} MjScript;

/* Open script. On failure returns NULL and the file is closed. */
MjScript* mj_script_open(struct Storage* storage, const char* path);

void mj_script_close(MjScript* s);

/* Run a single line. Updates `current_line` and (on warning) `last_warning`.
 * The caller is responsible for holding nrf24_hw_acquire across the call.
 * `abort` is polled inside long delays; if non-NULL and *abort becomes true
 * the line is aborted early and the function returns.
 * Returns true while the script has more lines, false on EOF/error. */
bool mj_script_step(MjScript* s, const MjTarget* target, volatile bool* abort);

/* True after the script has reached EOF (or unrecoverable I/O error). */
bool mj_script_done(const MjScript* s);

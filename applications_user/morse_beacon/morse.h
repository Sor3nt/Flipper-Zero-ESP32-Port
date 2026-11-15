#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MORSE_MAX_TEXT 64

/** One keyed interval: either tone-on (mark) or tone-off (space). */
typedef struct {
    bool on;
    uint32_t duration_us;
} MorseSegment;

typedef struct {
    MorseSegment* seg;
    size_t count;
    uint32_t total_us;
} MorseStream;

/** Dit length in microseconds for a PARIS-standard words-per-minute rate. */
uint32_t morse_dit_us(uint8_t wpm);

/** True if the character has a Morse representation (space counts). */
bool morse_char_supported(char c);

/** Pattern string ("-.-") for a character, or NULL. */
const char* morse_pattern(char c);

/** Render text to a timed segment stream.
 *
 * wpm        character speed
 * farnsworth overall speed; 0 or >= wpm sends at straight `wpm` spacing
 * preamble_ms/tail_ms  unkeyed carrier held before/after the message
 *
 * Returns NULL if the text contains nothing sendable.
 */
MorseStream* morse_stream_alloc(
    const char* text,
    uint8_t wpm,
    uint8_t farnsworth,
    uint16_t preamble_ms,
    uint16_t tail_ms);

void morse_stream_free(MorseStream* stream);

#include "morse.h"

#include <furi.h>
#include <string.h>

typedef struct {
    char c;
    const char* pattern;
} MorseTableEntry;

static const MorseTableEntry morse_table[] = {
    {'A', ".-"},      {'B', "-..."},    {'C', "-.-."},    {'D', "-.."},
    {'E', "."},       {'F', "..-."},    {'G', "--."},     {'H', "...."},
    {'I', ".."},      {'J', ".---"},    {'K', "-.-"},     {'L', ".-.."},
    {'M', "--"},      {'N', "-."},      {'O', "---"},     {'P', ".--."},
    {'Q', "--.-"},    {'R', ".-."},     {'S', "..."},     {'T', "-"},
    {'U', "..-"},     {'V', "...-"},    {'W', ".--"},     {'X', "-..-"},
    {'Y', "-.--"},    {'Z', "--.."},    {'0', "-----"},   {'1', ".----"},
    {'2', "..---"},   {'3', "...--"},   {'4', "....-"},   {'5', "....."},
    {'6', "-...."},   {'7', "--..."},   {'8', "---.."},   {'9', "----."},
    {'.', ".-.-.-"},  {',', "--..--"},  {'?', "..--.."},  {'\'', ".----."},
    {'!', "-.-.--"},  {'/', "-..-."},   {'(', "-.--."},   {')', "-.--.-"},
    {'&', ".-..."},   {':', "---..."},  {';', "-.-.-."},  {'=', "-...-"},
    {'+', ".-.-."},   {'-', "-....-"},  {'_', "..--.-"},  {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."},
};

#define MORSE_TABLE_SIZE  (sizeof(morse_table) / sizeof(morse_table[0]))
#define MORSE_MAX_SYMBOLS 7 // longest pattern above ($)

static char morse_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

const char* morse_pattern(char c) {
    c = morse_upper(c);
    for(size_t i = 0; i < MORSE_TABLE_SIZE; i++) {
        if(morse_table[i].c == c) return morse_table[i].pattern;
    }
    return NULL;
}

bool morse_char_supported(char c) {
    return c == ' ' || morse_pattern(c) != NULL;
}

uint32_t morse_dit_us(uint8_t wpm) {
    if(wpm < 1) wpm = 1;
    return 1200000UL / wpm;
}

static void morse_append(MorseStream* s, bool on, uint32_t duration_us) {
    if(duration_us == 0) return;
    s->seg[s->count].on = on;
    s->seg[s->count].duration_us = duration_us;
    s->count++;
    s->total_us += duration_us;
}

MorseStream* morse_stream_alloc(
    const char* text,
    uint8_t wpm,
    uint8_t farnsworth,
    uint16_t preamble_ms,
    uint16_t tail_ms) {
    furi_check(text);

    size_t len = strlen(text);
    if(len == 0) return NULL;
    if(len > MORSE_MAX_TEXT) len = MORSE_MAX_TEXT;

    const uint32_t dit_us = morse_dit_us(wpm);
    uint32_t char_gap_us = dit_us * 3;
    uint32_t word_gap_us = dit_us * 7;

    // Farnsworth: elements stay at `wpm`, the gaps stretch until the message
    // as a whole runs at `farnsworth` (ARRL timing).
    if(farnsworth > 0 && farnsworth < wpm) {
        float delay_s = (60.0f * wpm - 37.2f * farnsworth) / ((float)wpm * farnsworth);
        if(delay_s > 0.0f) {
            char_gap_us = (uint32_t)(delay_s * 3.0f / 19.0f * 1000000.0f);
            word_gap_us = (uint32_t)(delay_s * 7.0f / 19.0f * 1000000.0f);
        }
    }

    // worst case per character: leading gap + 7 marks + 6 intra-element gaps
    size_t capacity = len * (MORSE_MAX_SYMBOLS * 2 + 1) + 4;

    MorseStream* stream = malloc(sizeof(MorseStream));
    stream->seg = malloc(sizeof(MorseSegment) * capacity);
    stream->count = 0;
    stream->total_us = 0;

    morse_append(stream, false, (uint32_t)preamble_ms * 1000);

    uint32_t pending_gap_us = 0;
    bool any = false;

    for(size_t i = 0; i < len; i++) {
        char c = text[i];
        if(c == ' ' || c == '\t' || c == '\n') {
            if(any) pending_gap_us = word_gap_us;
            continue;
        }

        const char* pattern = morse_pattern(c);
        if(!pattern) continue;

        if(any) {
            morse_append(stream, false, pending_gap_us ? pending_gap_us : char_gap_us);
        }
        pending_gap_us = 0;

        for(const char* p = pattern; *p; p++) {
            if(p != pattern) morse_append(stream, false, dit_us);
            morse_append(stream, true, (*p == '-') ? dit_us * 3 : dit_us);
        }
        any = true;
    }

    if(!any) {
        morse_stream_free(stream);
        return NULL;
    }

    morse_append(stream, false, (uint32_t)tail_ms * 1000);

    furi_assert(stream->count <= capacity);
    return stream;
}

void morse_stream_free(MorseStream* stream) {
    if(!stream) return;
    free(stream->seg);
    free(stream);
}

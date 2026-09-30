#pragma once
#include <flipper_format/flipper_format.h>
#include <lib/flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <string.h>

/* The deployed ESP32 loader exports streams, but not flipper_format_delete_key.
 * Remove one complete matching line without changing unrelated capture fields. */
static inline bool pp_flipper_delete_key(FlipperFormat* ff, const char* key) {
    if(!ff || !key || !*key) return false;
    Stream* stream = flipper_format_get_raw_stream(ff);
    FuriString* line = furi_string_alloc();
    const size_t key_size = strlen(key);
    bool deleted = false;
    while(true) {
        size_t start = stream_tell(stream);
        if(!stream_read_line(stream, line)) break;
        size_t end = stream_tell(stream);
        const char* text = furi_string_get_cstr(line);
        if(strlen(text) > key_size && strncmp(text, key, key_size) == 0 && text[key_size] == ':') {
            deleted = stream_seek(stream, start, StreamOffsetFromStart) &&
                      stream_delete(stream, end - start);
            break;
        }
        if(end <= start) break;
    }
    furi_string_free(line);
    return deleted;
}

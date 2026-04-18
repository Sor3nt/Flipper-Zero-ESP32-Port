#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* relative_path;
    const uint8_t* data;
    size_t size;
} ExtDefaultEmbeddedFile;

extern const ExtDefaultEmbeddedFile ext_defaults_embedded_files[];
extern const size_t ext_defaults_embedded_files_count;

struct Storage;

/** If SD is mounted and /ext/dolphin/manifest.txt is missing, write bundled defaults. */
void storage_ext_try_seed_defaults(struct Storage* storage);

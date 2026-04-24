//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//     WAD I/O via the Flipper Storage service (SD card).
//     The original stdc backend used fopen/fread which do not work on
//     this ESP32 port because the SD card is mounted through FatFs
//     directly, bypassing the newlib VFS layer.
//

#include <string.h>

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

#include <furi.h>
#include <storage/storage.h>

typedef struct
{
    wad_file_t wad;
    Storage* storage;
    File* fstream;
} flipper_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_StdC_OpenFile(char *path)
{
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* fstream = storage_file_alloc(storage);

    /* Doom passes raw filesystem paths. If it is relative, rewrite to the
     * app data dir on the SD card. Absolute paths pass through unchanged
     * and rely on storage_map_path's fallback. */
    const char* open_path = path;
    char rewritten[256];
    if(path[0] != '/') {
        snprintf(rewritten, sizeof(rewritten), "/ext/apps_data/doom/%s", path);
        open_path = rewritten;
    }

    if(!storage_file_open(fstream, open_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E("Doom", "W_OpenFile: cannot open '%s'", open_path);
        storage_file_free(fstream);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    flipper_wad_file_t *result = Z_Malloc(sizeof(flipper_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = (unsigned int)storage_file_size(fstream);
    result->storage = storage;
    result->fstream = fstream;

    FURI_LOG_I("Doom", "W_OpenFile: '%s' (%u bytes)", open_path, result->wad.length);
    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad)
{
    flipper_wad_file_t *f = (flipper_wad_file_t *)wad;
    if(f->fstream) {
        storage_file_close(f->fstream);
        storage_file_free(f->fstream);
    }
    furi_record_close(RECORD_STORAGE);
    Z_Free(f);
}

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                   void *buffer, size_t buffer_len)
{
    flipper_wad_file_t *f = (flipper_wad_file_t *)wad;
    if(!storage_file_seek(f->fstream, offset, true)) {
        return 0;
    }
    return storage_file_read(f->fstream, buffer, buffer_len);
}

wad_file_class_t stdc_wad_file =
{
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};

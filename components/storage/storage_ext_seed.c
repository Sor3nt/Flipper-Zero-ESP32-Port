/**
 * Seed /ext defaults onto the SD card using FatFs directly (safe from the storage
 * service thread — must not use storage_file_* APIs there).
 */

#include "ext_defaults_embedded.h"

#include <fatfs.h>
#include <ff.h>
#include <furi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "StorageSeed";

#define SD_ROOT_PREFIX "0:/"

static void storage_seed_mkdirs_for_file(char* path_work) {
    const size_t prefix_len = strlen(SD_ROOT_PREFIX);
    for(char* s = path_work + prefix_len; *s; s++) {
        if(*s == '/') {
            *s = '\0';
            f_mkdir(path_work);
            *s = '/';
        }
    }
}

void storage_ext_try_seed_defaults(struct Storage* storage) {
    (void)storage;

    /* /ext in the VFS is the root of the SD volume (paths become 0:/subghz/..., not 0:/ext/...).
     * Fill in any missing bundled files so SubGHz / IR / NFC databases work without manual copies. */
    for(size_t i = 0; i < ext_defaults_embedded_files_count; i++) {
        const ExtDefaultEmbeddedFile* ent = &ext_defaults_embedded_files[i];
        char path[256];
        int n = snprintf(path, sizeof(path), SD_ROOT_PREFIX "%s", ent->relative_path);
        if(n < 0 || (size_t)n >= sizeof(path)) {
            FURI_LOG_E(TAG, "Seed path too long");
            continue;
        }

        FILINFO fno;
        if(f_stat(path, &fno) == FR_OK) {
            continue;
        }

        FURI_LOG_I(TAG, "Seeding missing SD file %s", path);

        char path_work[256];
        strncpy(path_work, path, sizeof(path_work) - 1);
        path_work[sizeof(path_work) - 1] = '\0';
        storage_seed_mkdirs_for_file(path_work);

        FIL file;
        FRESULT open_res =
            f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS);
        if(open_res == FR_OK) {
            /* fatfs.h f_write maps to fatfs_compat_write(..., uint16_t* bytes_written) */
            const uint8_t* p = ent->data;
            size_t remain = ent->size;
            bool write_ok = true;
            while(remain > 0 && write_ok) {
                UINT chunk = (remain > 65535u) ? 65535u : (UINT)remain;
                uint16_t written = 0;
                FRESULT wr = f_write(&file, p, chunk, &written);
                if(wr != FR_OK || (UINT)written != chunk) {
                    write_ok = false;
                    break;
                }
                p += written;
                remain -= written;
            }
            f_close(&file);
            if(!write_ok || remain != 0) {
                FURI_LOG_E(TAG, "Failed writing seed file %s", path);
            }
        } else {
            FURI_LOG_E(TAG, "Failed opening seed file %s (fr=%d)", path, (int)open_res);
        }
    }

    f_mkdir(SD_ROOT_PREFIX "asset_packs");
}

#include "md5_calc.h"

#include <mbedtls/md5.h>
#include <storage/filesystem_api_defines.h>
#include <storage/storage.h>

bool md5_calc_file(File* file, const char* path, unsigned char output[16], FS_Error* file_error) {
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(file_error != NULL) {
            *file_error = storage_file_get_error(file);
        }
        return false;
    }

    const size_t size_to_read = 512;
    uint8_t* data = malloc(size_to_read);
    bool result = data != NULL;

    mbedtls_md5_context* md5_ctx = NULL;
    if(result) {
        md5_ctx = malloc(sizeof(mbedtls_md5_context));
        result = md5_ctx != NULL;
    }

    if(result) {
        mbedtls_md5_init(md5_ctx);
        mbedtls_md5_starts(md5_ctx);
        while(true) {
            const size_t read_size = storage_file_read(file, data, size_to_read);
            if(storage_file_get_error(file) != FSE_OK) {
                result = false;
                break;
            }
            if(read_size == 0) {
                break;
            }
            mbedtls_md5_update(md5_ctx, data, read_size);
        }
        if(result) {
            mbedtls_md5_finish(md5_ctx, output);
        }
    }

    if(md5_ctx != NULL) {
        mbedtls_md5_free(md5_ctx);
        free(md5_ctx);
    }
    free(data);

    if(file_error != NULL) {
        *file_error = storage_file_get_error(file);
    }

    storage_file_close(file);
    return result;
}

bool md5_string_calc_file(File* file, const char* path, FuriString* output, FS_Error* file_error) {
    const size_t hash_size = 16;
    unsigned char hash[hash_size];
    const bool result = md5_calc_file(file, path, hash, file_error);

    if(result) {
        furi_string_set(output, "");
        for(size_t i = 0; i < hash_size; i++) {
            furi_string_cat_printf(output, "%02x", hash[i]);
        }
    }

    return result;
}

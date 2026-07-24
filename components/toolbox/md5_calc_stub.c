/**
 * \file md5_calc_stub.c
 * \brief Stub implementation for md5_calc functions
 * 
 * MD5 is not available in ESP-IDF 6.2 TF-PSA Crypto.
 */

#include "toolbox/md5_calc.h"
#include <string.h>

bool md5_string_calc_file(File* file, const char* path, FuriString* output, FS_Error* file_error) {
    (void)file;
    (void)path;
    (void)output;
    (void)file_error;
    /* MD5 not available - return zero hash */
    if(output) {
        furi_string_set(output, "00000000000000000000000000000000");
    }
    return false;
}

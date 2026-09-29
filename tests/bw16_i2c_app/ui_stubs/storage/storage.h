#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../../../../applications/services/storage/filesystem_api_defines.h"
#define RECORD_STORAGE "storage"
typedef struct Storage Storage;
File* storage_file_alloc(Storage*);
void storage_file_free(File*);
bool storage_simply_mkdir(Storage*, const char*);
bool storage_file_open(File*, const char*, FS_AccessMode, FS_OpenMode);
bool storage_file_close(File*);
bool storage_file_sync(File*);
FS_Error storage_file_get_error(File*);
size_t storage_file_write(File*, const void*, size_t);

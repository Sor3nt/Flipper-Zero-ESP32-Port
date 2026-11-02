#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest blob the helpers handle (kept on a small internal-RAM stack). */
#define FURI_HAL_NVS_BLOB_MAX 32U

/** Read a small blob. Succeeds only when the stored size equals `size`. Safe from any thread. */
bool furi_hal_nvs_blob_read(const char* ns, const char* key, void* data, size_t size);

/** Store a small blob (committed before returning). Safe from any thread, including the Furi
 *  threads whose stacks live in PSRAM: nvs_commit() switches the flash cache off, so the
 *  write runs in a plain task with an internal stack that only touches internal RAM. */
bool furi_hal_nvs_blob_write(const char* ns, const char* key, const void* data, size_t size);

#ifdef __cplusplus
}
#endif

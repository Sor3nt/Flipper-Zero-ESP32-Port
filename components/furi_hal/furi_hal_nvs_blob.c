#include "furi_hal_nvs_blob.h"

#include <string.h>
#include <esp_log.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define NVS_NAME_MAX 15U /* NVS namespace/key limit */

static const char* TAG = "FuriHalNvsBlob";

typedef struct {
    char ns[NVS_NAME_MAX + 1];
    char key[NVS_NAME_MAX + 1];
    uint8_t data[FURI_HAL_NVS_BLOB_MAX];
    size_t size;
    volatile bool ok;
    volatile bool done;
} FuriHalNvsBlobWrite;

static void furi_hal_nvs_blob_write_task(void* arg) {
    FuriHalNvsBlobWrite* job = arg;
    /* Copy everything out of the caller's (possibly PSRAM) job before any flash access. */
    char ns[NVS_NAME_MAX + 1];
    char key[NVS_NAME_MAX + 1];
    uint8_t data[FURI_HAL_NVS_BLOB_MAX];
    const size_t size = job->size;
    memcpy(ns, job->ns, sizeof(ns));
    memcpy(key, job->key, sizeof(key));
    memcpy(data, job->data, size);

    bool ok = false;
    nvs_handle_t handle;
    if(nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_blob(handle, key, data, size) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    job->ok = ok;
    job->done = true;
    vTaskDelete(NULL);
}

bool furi_hal_nvs_blob_read(const char* ns, const char* key, void* data, size_t size) {
    if(!ns || !key || !data || size == 0 || size > FURI_HAL_NVS_BLOB_MAX) return false;
    nvs_handle_t handle;
    if(nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t blob[FURI_HAL_NVS_BLOB_MAX];
    size_t stored = sizeof(blob);
    const bool ok = nvs_get_blob(handle, key, blob, &stored) == ESP_OK && stored == size;
    nvs_close(handle);
    if(ok) memcpy(data, blob, size);
    return ok;
}

bool furi_hal_nvs_blob_write(const char* ns, const char* key, const void* data, size_t size) {
    if(!ns || !key || !data || size == 0 || size > FURI_HAL_NVS_BLOB_MAX ||
       strlen(ns) > NVS_NAME_MAX || strlen(key) > NVS_NAME_MAX) {
        return false;
    }
    FuriHalNvsBlobWrite job = {.size = size, .ok = false, .done = false};
    strlcpy(job.ns, ns, sizeof(job.ns));
    strlcpy(job.key, key, sizeof(job.key));
    memcpy(job.data, data, size);

    if(xTaskCreate(furi_hal_nvs_blob_write_task, "nvs_blob", 3072, &job, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the settings write for %s/%s", ns, key);
        return false;
    }
    while(!job.done) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if(!job.ok) ESP_LOGE(TAG, "Storing %s/%s failed", ns, key);
    return job.ok;
}

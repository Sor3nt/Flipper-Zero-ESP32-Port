#include "nrf24_wifi_scan.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Nrf24WifiScan"

static bool s_netif_initialized = false;

/* Shared request block. Heap-allocated and reference-counted (caller + task =
 * 2 refs) so it survives a caller-side timeout: if the scan task outlives the
 * 8 s wait, the caller must NOT free the block or the semaphores while the task
 * is still writing into it. Whoever releases the last reference frees the block.
 * All records live inside `records` here (not in the caller's stack) so a late
 * task can never dereference a popped stack frame. */
typedef struct {
    wifi_ap_record_t* records; /* task-owned until handed to caller */
    uint16_t count;
    uint16_t max_count;
    bool result;
    SemaphoreHandle_t done; /* signals results ready */
    SemaphoreHandle_t lock; /* guards refs + records ownership handoff */
    uint8_t refs; /* 2 initially: caller + task; last to release frees */
} ScanReq;

/* Drop one reference; the party that takes it to zero frees everything. */
static void scanreq_release(ScanReq* req) {
    xSemaphoreTake(req->lock, portMAX_DELAY);
    uint8_t remaining = --req->refs;
    xSemaphoreGive(req->lock);
    if(remaining == 0) {
        if(req->records) free(req->records);
        vSemaphoreDelete(req->done);
        vSemaphoreDelete(req->lock);
        free(req);
    }
}

static bool wifi_init_once(void) {
    esp_err_t err = esp_netif_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create: %s", esp_err_to_name(err));
        return false;
    }
    /* Only create the default STA netif if another app hasn't already done so;
     * esp_netif_create_default_wifi_sta() asserts on duplicate keys. */
    if(!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    s_netif_initialized = true;
    return true;
}

/* Runs as a real FreeRTOS task — esp_wifi_* requires a proper pthread/TLS
 * context that FuriThread wrappers do not provide. */
static void wifi_scan_task(void* arg) {
    ScanReq* req = arg;
    req->records = NULL;
    req->count = 0;
    req->result = false;

    if(!wifi_init_once()) goto done;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 4;

    esp_err_t err = esp_wifi_init(&cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
        goto done;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        goto done;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        esp_wifi_stop();
        esp_wifi_deinit();
        goto done;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if(count > req->max_count) count = req->max_count;

    if(count > 0) {
        req->records = malloc(count * sizeof(wifi_ap_record_t));
        if(req->records) {
            esp_wifi_scan_get_ap_records(&count, req->records);
            req->count = count;
        } else {
            count = 0;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    req->result = true;

done:
    /* Publish results (all writes above happen-before this give), then drop our
     * reference. Nothing below touches req, so a caller woken by this give may
     * safely read req under the lock. */
    xSemaphoreGive(req->done);
    scanreq_release(req);
    vTaskDelete(NULL);
}

bool nrf24_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    *out_records = NULL;
    *out_count = 0;

    ScanReq* req = malloc(sizeof(ScanReq));
    if(!req) return false;
    memset(req, 0, sizeof(*req));
    req->max_count = max_count;
    req->refs = 2;
    req->done = xSemaphoreCreateBinary();
    req->lock = xSemaphoreCreateMutex();
    if(!req->done || !req->lock) {
        if(req->done) vSemaphoreDelete(req->done);
        if(req->lock) vSemaphoreDelete(req->lock);
        free(req);
        return false;
    }

    BaseType_t ok =
        xTaskCreatePinnedToCore(wifi_scan_task, "Nrf24WifiScan", 6 * 1024, req, 5, NULL, 0);
    if(ok != pdPASS) {
        /* Task never started, so no second reference exists — free directly. */
        vSemaphoreDelete(req->done);
        vSemaphoreDelete(req->lock);
        free(req);
        return false;
    }

    /* Scan typically takes ~3 s; cap at 8 s to be safe. CRITICAL: on timeout the
     * task is still alive and still owns a reference to `req`, so we must NOT
     * free anything here — we only drop our own reference and return failure.
     * The task frees the block when it eventually finishes. */
    bool took = (xSemaphoreTake(req->done, pdMS_TO_TICKS(8000)) == pdTRUE);

    bool result = false;
    if(took) {
        /* Task has published results and will not touch req's data fields again
         * (it only calls scanreq_release next). Claim the records under the lock
         * so the final free does not also free them. */
        xSemaphoreTake(req->lock, portMAX_DELAY);
        result = req->result;
        *out_records = req->records;
        *out_count = req->count;
        req->records = NULL; /* ownership transferred to caller */
        xSemaphoreGive(req->lock);
    } else {
        ESP_LOGW(TAG, "wifi scan timed out after 8s; abandoning task");
    }

    scanreq_release(req);
    return took && result;
}

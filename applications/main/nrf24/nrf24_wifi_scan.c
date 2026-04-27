#include "nrf24_wifi_scan.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>

#define TAG "Nrf24WifiScan"

static bool s_netif_initialized = false;

typedef struct {
    wifi_ap_record_t** out_records;
    uint16_t* out_count;
    uint16_t max_count;
    bool result;
    SemaphoreHandle_t done;
} ScanReq;

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
    *req->out_records = NULL;
    *req->out_count = 0;
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
        *req->out_records = malloc(count * sizeof(wifi_ap_record_t));
        if(*req->out_records) {
            esp_wifi_scan_get_ap_records(&count, *req->out_records);
            *req->out_count = count;
        } else {
            count = 0;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    req->result = true;

done:
    xSemaphoreGive(req->done);
    vTaskDelete(NULL);
}

bool nrf24_wifi_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    *out_records = NULL;
    *out_count = 0;

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if(!done) return false;

    ScanReq req = {
        .out_records = out_records,
        .out_count = out_count,
        .max_count = max_count,
        .result = false,
        .done = done,
    };

    BaseType_t ok = xTaskCreatePinnedToCore(
        wifi_scan_task, "Nrf24WifiScan", 6 * 1024, &req, 5, NULL, 0);
    if(ok != pdPASS) {
        vSemaphoreDelete(done);
        return false;
    }

    /* Scan typically takes ~3 s; cap at 8 s to be safe */
    xSemaphoreTake(done, pdMS_TO_TICKS(8000));
    vSemaphoreDelete(done);

    return req.result;
}

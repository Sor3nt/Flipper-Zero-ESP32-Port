#include "buddy_espnow.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "buddy_config.h"

static const char* TAG = "buddy-now";

/* RX hand-off: the ESP-NOW recv callback runs in the WiFi task and must stay
 * light, so it only copies the frame into a queue. The worker task drains the
 * queue and invokes the user callback where blocking/sending is allowed. */
typedef struct {
    uint8_t mac[BUDDY_MAC_LEN];
    uint8_t buf[BUDDY_MAX_PAYLOAD];
    int len;
} rx_item_t;

#define RX_QLEN 8

static QueueHandle_t s_rx_q;
static buddy_frame_cb_t s_cb;
static void* s_cb_ctx;
static uint8_t s_self_mac[BUDDY_MAC_LEN];
static volatile TickType_t s_last_send_ok; /* Tick des letzten ACK'd Sends */

static void on_send(const uint8_t* mac, esp_now_send_status_t status) {
    (void)mac;
    if(status == ESP_NOW_SEND_SUCCESS) {
        s_last_send_ok = xTaskGetTickCount();
    } else {
        ESP_LOGD(TAG, "send failed");
    }
}

uint32_t buddy_espnow_ms_since_send_ok(void) {
    return (uint32_t)((xTaskGetTickCount() - s_last_send_ok) * portTICK_PERIOD_MS);
}

void buddy_espnow_note_alive(void) {
    s_last_send_ok = xTaskGetTickCount();
}

static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if(!info || !data || len < 2 || len > BUDDY_MAX_PAYLOAD) return;
    if(data[0] != BUDDY_MAGIC) return;

    rx_item_t it;
    memcpy(it.mac, info->src_addr, BUDDY_MAC_LEN);
    memcpy(it.buf, data, len);
    it.len = len;
    /* Drop on overflow rather than block the WiFi task. */
    if(s_rx_q) xQueueSend(s_rx_q, &it, 0);
}

static void worker_task(void* arg) {
    (void)arg;
    rx_item_t it;
    for(;;) {
        if(xQueueReceive(s_rx_q, &it, portMAX_DELAY) != pdTRUE) continue;
        if(s_cb) s_cb(it.mac, it.buf, it.len, s_cb_ctx);
    }
}

void buddy_espnow_ensure_peer(const uint8_t mac[BUDDY_MAC_LEN]) {
    if(esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t pi = {0};
    memcpy(pi.peer_addr, mac, BUDDY_MAC_LEN);
    pi.channel = 0; /* 0 = aktueller Radio-Kanal — beim Capture sind wir nicht auf ch1 */
    pi.ifidx = WIFI_IF_STA;
    pi.encrypt = false;
    esp_err_t e = esp_now_add_peer(&pi);
    if(e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "add_peer: %s", esp_err_to_name(e));
    }
}

bool buddy_espnow_send(const uint8_t mac[BUDDY_MAC_LEN], const uint8_t* data, uint8_t len) {
    if(len > BUDDY_MAX_PAYLOAD) return false;
    buddy_espnow_ensure_peer(mac);
    esp_err_t e = esp_now_send(mac, data, len);
    if(e != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

void buddy_espnow_get_self_mac(uint8_t out[BUDDY_MAC_LEN]) {
    memcpy(out, s_self_mac, BUDDY_MAC_LEN);
}

bool buddy_espnow_init(buddy_frame_cb_t cb, void* ctx) {
    s_cb = cb;
    s_cb_ctx = ctx;
    s_last_send_ok = xTaskGetTickCount();

    s_rx_q = xQueueCreate(RX_QLEN, sizeof(rx_item_t));
    if(!s_rx_q) {
        ESP_LOGE(TAG, "rx queue alloc failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if(e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Großzügige TX-Puffer: beim pcap-Streaming feuern wir viele kleine
     * Fragmente in Bursts — ein zu kleiner TX-Pool gibt ESP_ERR_ESPNOW_NO_MEM
     * und verliert Frames. (Defaults sind 32/32; klein gedrosselt war zu wenig.) */
    cfg.static_rx_buf_num = 8;
    cfg.dynamic_rx_buf_num = 24;
    cfg.dynamic_tx_buf_num = 32;
    if(esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "wifi_init failed");
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if(esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed");
        return false;
    }
    esp_wifi_set_channel(BUDDY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, s_self_mac);

    if(esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);

    if(xTaskCreate(worker_task, "buddy_now", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker task alloc failed");
        return false;
    }

    ESP_LOGI(
        TAG,
        "ready ch=%d mac=%02X:%02X:%02X:%02X:%02X:%02X",
        BUDDY_ESPNOW_CHANNEL,
        s_self_mac[0],
        s_self_mac[1],
        s_self_mac[2],
        s_self_mac[3],
        s_self_mac[4],
        s_self_mac[5]);
    return true;
}

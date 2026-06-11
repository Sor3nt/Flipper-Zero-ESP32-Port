/* ESP32 Buddy — headless ESP-NOW client for the Flipper-Zero-ESP32 master.
 *
 * Boot order:
 *   1. NVS (also a WiFi prerequisite)
 *   2. node init  — derive device name, load any stored master
 *   3. ESP-NOW    — WiFi STA + ESP-NOW up, frames pumped to buddy_node_on_frame
 *   4. node ready — pre-register stored master, log status
 *
 * After that everything is event driven: the buddy worker task handles incoming
 * master commands and feature tasks run on their own. app_main returns; the
 * FreeRTOS scheduler keeps the firmware alive. */

#include <stddef.h>

#include "esp_log.h"

#include "buddy_espnow.h"
#include "buddy_node.h"
#include "buddy_store.h"

static const char* TAG = "buddy";

void app_main(void) {
    buddy_store_init();
    buddy_node_init();

    if(!buddy_espnow_init(buddy_node_on_frame, NULL)) {
        ESP_LOGE(TAG, "ESP-NOW init failed — halting");
        return;
    }

    buddy_node_ready();
    ESP_LOGI(TAG, "waiting for master discovery…");
}

#include "buddy_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "buddy-store";

#define NVS_NS  "buddy"
#define NVS_KEY "master"

typedef struct __attribute__((packed)) {
    uint8_t mac[BUDDY_MAC_LEN];
    char name[BUDDY_NAME_MAX + 1];
} stored_master_t;

void buddy_store_init(void) {
    esp_err_t e = nvs_flash_init();
    if(e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase + reinit (%s)", esp_err_to_name(e));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(e);
    }
}

bool buddy_store_load_master(BuddyMaster* out) {
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    stored_master_t s;
    size_t sz = sizeof(s);
    esp_err_t e = nvs_get_blob(h, NVS_KEY, &s, &sz);
    nvs_close(h);
    if(e != ESP_OK || sz != sizeof(s)) return false;

    memcpy(out->mac, s.mac, BUDDY_MAC_LEN);
    s.name[BUDDY_NAME_MAX] = '\0';
    strlcpy(out->name, s.name, sizeof(out->name));
    out->valid = true;
    return true;
}

bool buddy_store_save_master(const uint8_t mac[BUDDY_MAC_LEN], const char* name) {
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    stored_master_t s = {0};
    memcpy(s.mac, mac, BUDDY_MAC_LEN);
    strlcpy(s.name, name ? name : "", sizeof(s.name));

    esp_err_t e = nvs_set_blob(h, NVS_KEY, &s, sizeof(s));
    if(e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if(e != ESP_OK) ESP_LOGW(TAG, "save master: %s", esp_err_to_name(e));
    return e == ESP_OK;
}

void buddy_store_clear_master(void) {
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY);
    nvs_commit(h);
    nvs_close(h);
}

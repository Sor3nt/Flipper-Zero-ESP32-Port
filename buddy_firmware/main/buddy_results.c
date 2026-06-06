#include "buddy_results.h"

#include <string.h>

#include "esp_log.h"

#include "buddy_espnow.h"

#define TAG "buddy-res"

typedef struct {
    bool used;
    uint8_t id;
    uint8_t type;
    uint8_t len;
    uint8_t data[BUDDY_RESULT_DATA_MAX];
} ResultSlot;

static ResultSlot s_slots[BUDDY_RESULT_MAX];
static uint8_t s_next_id = 1; /* 0 is reserved/invalid */

void buddy_results_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;
}

static uint8_t alloc_id(void) {
    uint8_t id = s_next_id++;
    if(s_next_id == 0) s_next_id = 1; /* skip 0 on wrap */
    return id;
}

bool buddy_results_push(uint8_t type, const uint8_t* data, uint8_t len) {
    if(len > BUDDY_RESULT_DATA_MAX) len = BUDDY_RESULT_DATA_MAX;

    ResultSlot* slot = NULL;
    for(int i = 0; i < BUDDY_RESULT_MAX; ++i) {
        if(!s_slots[i].used) {
            slot = &s_slots[i];
            break;
        }
    }
    if(!slot) {
        /* Full → evict the oldest pending result (lowest id, modulo the rare wrap). */
        slot = &s_slots[0];
        for(int i = 1; i < BUDDY_RESULT_MAX; ++i) {
            if(s_slots[i].id < slot->id) slot = &s_slots[i];
        }
        ESP_LOGW(TAG, "result queue full, evicting id=%u", slot->id);
    }

    slot->used = true;
    slot->id = alloc_id();
    slot->type = type;
    slot->len = len;
    if(data && len) memcpy(slot->data, data, len);
    ESP_LOGI(TAG, "result queued id=%u type=%u len=%u", slot->id, type, len);
    return true;
}

void buddy_results_pump(const uint8_t master_mac[BUDDY_MAC_LEN]) {
    for(int i = 0; i < BUDDY_RESULT_MAX; ++i) {
        ResultSlot* s = &s_slots[i];
        if(!s->used) continue;
        uint8_t o[BUDDY_MAX_PAYLOAD];
        o[0] = BUDDY_MAGIC;
        o[1] = BuddyWireResult;
        o[2] = s->id;
        o[3] = s->type;
        o[4] = s->len;
        if(s->len) memcpy(&o[5], s->data, s->len);
        buddy_espnow_send(master_mac, o, (uint8_t)(5 + s->len));
    }
}

void buddy_results_ack(uint8_t id) {
    for(int i = 0; i < BUDDY_RESULT_MAX; ++i) {
        if(s_slots[i].used && s_slots[i].id == id) {
            s_slots[i].used = false;
            ESP_LOGI(TAG, "result id=%u acked", id);
            return;
        }
    }
}

bool buddy_results_has_pending(void) {
    for(int i = 0; i < BUDDY_RESULT_MAX; ++i)
        if(s_slots[i].used) return true;
    return false;
}

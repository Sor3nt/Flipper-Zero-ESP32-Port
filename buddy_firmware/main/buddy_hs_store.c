#include "buddy_hs_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs.h"

#include "buddy_espnow.h"

#define TAG "buddy-hs"

#define NVS_NS   "buddy_hs"
#define NVS_KEY  "pending"
#define NVS_VER  1

/* Wire-Result-Payload (reassembliert beim Master):
 *   [type:1][ssid_len:1][ssid][frame_count:1]{ [msg_num:1][len:2 LE][bytes] } */
#define WIRE_CHUNK (BUDDY_MAX_PAYLOAD - 5) /* magic,type,id,frag_idx,frag_cnt */

typedef struct {
    bool used;
    bool complete; /* M2 & M3 vorhanden */
    uint8_t id; /* lokale id für ResultAck */
    uint8_t bssid[6];
    char ssid[33];
    uint16_t len[HS_SLOTS]; /* 0 = Slot leer */
    uint8_t frame[HS_SLOTS][HS_FRAME_MAX];
} HsRecord;

static HsRecord s_rec[HS_MAX_RECORDS];
static uint8_t s_next_id = 1;
static SemaphoreHandle_t s_mtx;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static uint8_t alloc_id(void) {
    uint8_t id = s_next_id++;
    if(s_next_id == 0) s_next_id = 1;
    return id;
}

static HsRecord* find(const uint8_t bssid[6]) {
    for(int i = 0; i < HS_MAX_RECORDS; ++i)
        if(s_rec[i].used && memcmp(s_rec[i].bssid, bssid, 6) == 0) return &s_rec[i];
    return NULL;
}

static HsRecord* find_or_add(const uint8_t bssid[6]) {
    HsRecord* r = find(bssid);
    if(r) return r;
    for(int i = 0; i < HS_MAX_RECORDS; ++i) {
        if(!s_rec[i].used) {
            r = &s_rec[i];
            memset(r, 0, sizeof(*r));
            r->used = true;
            memcpy(r->bssid, bssid, 6);
            return r;
        }
    }
    /* voll → einen unvollständigen Record evicten (vollständige nicht verlieren). */
    for(int i = 0; i < HS_MAX_RECORDS; ++i) {
        if(!s_rec[i].complete) {
            r = &s_rec[i];
            memset(r, 0, sizeof(*r));
            r->used = true;
            memcpy(r->bssid, bssid, 6);
            return r;
        }
    }
    return NULL; /* alle Slots vollständig + ungeackt → neuen verwerfen */
}

static void store_frame(HsRecord* r, uint8_t slot, const uint8_t* frame, uint16_t len) {
    if(slot >= HS_SLOTS || !frame || len == 0) return;
    if(len > HS_FRAME_MAX) len = HS_FRAME_MAX;
    memcpy(r->frame[slot], frame, len);
    r->len[slot] = len;
}

/* ─────── NVS-Persistenz: alle vollständigen Records als ein Blob ─────── */
static void persist(void) {
    /* static (nicht Stack!): ~5 KB würden den cap_hs-Task-Stack sprengen. Zugriff
     * immer unter LOCK → serialisiert, also sicher. */
    static uint8_t buf[HS_MAX_RECORDS * (8 + 33 + 1 + HS_SLOTS * (3 + HS_FRAME_MAX)) + 2];
    size_t o = 0;
    buf[o++] = NVS_VER;
    size_t cnt_off = o++;
    uint8_t cnt = 0;

    for(int i = 0; i < HS_MAX_RECORDS; ++i) {
        HsRecord* r = &s_rec[i];
        if(!r->used || !r->complete) continue;
        memcpy(&buf[o], r->bssid, 6);
        o += 6;
        uint8_t sl = (uint8_t)strnlen(r->ssid, 32);
        buf[o++] = sl;
        memcpy(&buf[o], r->ssid, sl);
        o += sl;
        size_t mask_off = o++;
        uint8_t mask = 0;
        for(uint8_t s = 0; s < HS_SLOTS; ++s) {
            if(!r->len[s]) continue;
            mask |= (1u << s);
            buf[o++] = (uint8_t)(r->len[s] & 0xff);
            buf[o++] = (uint8_t)(r->len[s] >> 8);
            memcpy(&buf[o], r->frame[s], r->len[s]);
            o += r->len[s];
        }
        buf[mask_off] = mask;
        cnt++;
    }
    buf[cnt_off] = cnt;

    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if(cnt == 0) {
        nvs_erase_key(h, NVS_KEY);
    } else {
        nvs_set_blob(h, NVS_KEY, buf, o);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load(void) {
    nvs_handle_t h;
    if(nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    static uint8_t buf[HS_MAX_RECORDS * (8 + 33 + 1 + HS_SLOTS * (3 + HS_FRAME_MAX)) + 2];
    size_t sz = sizeof(buf);
    esp_err_t e = nvs_get_blob(h, NVS_KEY, buf, &sz);
    nvs_close(h);
    if(e != ESP_OK || sz < 2 || buf[0] != NVS_VER) return;

    size_t o = 1;
    uint8_t cnt = buf[o++];
    for(uint8_t i = 0; i < cnt && i < HS_MAX_RECORDS; ++i) {
        if(o + 8 > sz) break;
        HsRecord* r = &s_rec[i];
        memset(r, 0, sizeof(*r));
        r->used = true;
        r->complete = true;
        r->id = alloc_id();
        memcpy(r->bssid, &buf[o], 6);
        o += 6;
        uint8_t sl = buf[o++];
        if(sl > 32 || o + sl > sz) break;
        memcpy(r->ssid, &buf[o], sl);
        r->ssid[sl] = '\0';
        o += sl;
        uint8_t mask = buf[o++];
        for(uint8_t s = 0; s < HS_SLOTS; ++s) {
            if(!(mask & (1u << s))) continue;
            if(o + 2 > sz) break;
            uint16_t flen = (uint16_t)(buf[o] | (buf[o + 1] << 8));
            o += 2;
            if(flen > HS_FRAME_MAX || o + flen > sz) break;
            memcpy(r->frame[s], &buf[o], flen);
            r->len[s] = flen;
            o += flen;
        }
    }
    ESP_LOGI(TAG, "loaded %u pending handshake(s) from NVS", cnt);
}

/* ─────── public ─────── */
void buddy_hs_store_init(void) {
    if(!s_mtx) s_mtx = xSemaphoreCreateMutex();
    LOCK();
    memset(s_rec, 0, sizeof(s_rec));
    s_next_id = 1;
    load();
    UNLOCK();
}

void buddy_hs_store_beacon(
    const uint8_t bssid[6],
    const char* ssid,
    const uint8_t* frame,
    uint16_t len) {
    LOCK();
    HsRecord* r = find(bssid); /* Beacon legt KEINEN neuen Record an (sonst Beacon-Flut) */
    if(r) {
        if(ssid && ssid[0] && !r->ssid[0]) strlcpy(r->ssid, ssid, sizeof(r->ssid));
        store_frame(r, 0, frame, len);
        if(r->complete) persist();
    }
    UNLOCK();
}

bool buddy_hs_store_eapol(
    const uint8_t bssid[6],
    const char* ssid,
    uint8_t msg_num,
    const uint8_t* frame,
    uint16_t len) {
    if(msg_num < 1 || msg_num > 4) return false;
    bool newly_complete = false;
    LOCK();
    HsRecord* r = find_or_add(bssid);
    if(r) {
        if(ssid && ssid[0] && !r->ssid[0]) strlcpy(r->ssid, ssid, sizeof(r->ssid));
        store_frame(r, msg_num, frame, len);
        bool now = (r->len[2] && r->len[3]); /* M2 & M3 */
        if(now && !r->complete) {
            r->complete = true;
            r->id = alloc_id();
            newly_complete = true;
            persist();
            ESP_LOGI(TAG, "handshake complete id=%u ssid='%s'", r->id, r->ssid);
        }
    }
    UNLOCK();
    return newly_complete;
}

void buddy_hs_store_pump(const uint8_t master_mac[6]) {
    LOCK();
    for(int i = 0; i < HS_MAX_RECORDS; ++i) {
        HsRecord* r = &s_rec[i];
        if(!r->used || !r->complete) continue;

        /* Result-Payload serialisieren. */
        static uint8_t blob[1 + 1 + 33 + 1 + HS_SLOTS * (3 + HS_FRAME_MAX)];
        size_t n = 0;
        blob[n++] = BuddyResultHandshake;
        uint8_t sl = (uint8_t)strnlen(r->ssid, 32);
        blob[n++] = sl;
        memcpy(&blob[n], r->ssid, sl);
        n += sl;
        size_t fc_off = n++;
        uint8_t fc = 0;
        for(uint8_t s = 0; s < HS_SLOTS; ++s) {
            if(!r->len[s]) continue;
            blob[n++] = s;
            blob[n++] = (uint8_t)(r->len[s] & 0xff);
            blob[n++] = (uint8_t)(r->len[s] >> 8);
            memcpy(&blob[n], r->frame[s], r->len[s]);
            n += r->len[s];
            fc++;
        }
        blob[fc_off] = fc;

        /* fragmentiert senden. */
        uint8_t cnt = (uint8_t)((n + WIRE_CHUNK - 1) / WIRE_CHUNK);
        if(cnt == 0) cnt = 1;
        for(uint8_t idx = 0; idx < cnt; ++idx) {
            uint8_t o[BUDDY_MAX_PAYLOAD];
            o[0] = BUDDY_MAGIC;
            o[1] = BuddyWireResult;
            o[2] = r->id;
            o[3] = idx;
            o[4] = cnt;
            size_t off = (size_t)idx * WIRE_CHUNK;
            size_t c = n - off;
            if(c > WIRE_CHUNK) c = WIRE_CHUNK;
            memcpy(&o[5], &blob[off], c);
            buddy_espnow_send(master_mac, o, (uint8_t)(5 + c));
            vTaskDelay(pdMS_TO_TICKS(3)); /* TX pacen */
        }
    }
    UNLOCK();
}

void buddy_hs_store_ack(uint8_t id) {
    LOCK();
    for(int i = 0; i < HS_MAX_RECORDS; ++i) {
        if(s_rec[i].used && s_rec[i].complete && s_rec[i].id == id) {
            memset(&s_rec[i], 0, sizeof(s_rec[i]));
            persist();
            ESP_LOGI(TAG, "handshake id=%u acked + cleared", id);
            break;
        }
    }
    UNLOCK();
}

bool buddy_hs_store_has_pending(void) {
    bool any = false;
    LOCK();
    for(int i = 0; i < HS_MAX_RECORDS; ++i)
        if(s_rec[i].used && s_rec[i].complete) {
            any = true;
            break;
        }
    UNLOCK();
    return any;
}

uint8_t buddy_hs_store_complete_count(void) {
    uint8_t c = 0;
    LOCK();
    for(int i = 0; i < HS_MAX_RECORDS; ++i)
        if(s_rec[i].used && s_rec[i].complete) c++;
    UNLOCK();
    return c;
}

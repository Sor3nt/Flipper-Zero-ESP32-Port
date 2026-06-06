#include "buddy_node.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "buddy_config.h"
#include "buddy_espnow.h"
#include "buddy_features.h"
#include "buddy_hs_store.h"
#include "buddy_store.h"

static const char* TAG = "buddy-node";

static char s_name[BUDDY_NAME_MAX + 1];
static BuddyMaster s_master; /* the master we are paired with (if .valid) */

/* ─────── wire helpers ─────── */

static uint8_t pack_name(uint8_t* b, uint8_t off, const char* name) {
    size_t n = strnlen(name, BUDDY_NAME_MAX);
    b[off++] = (uint8_t)n;
    memcpy(&b[off], name, n);
    return off + (uint8_t)n;
}

static uint8_t pack_caps(uint8_t* b, uint8_t off, uint32_t caps) {
    b[off++] = (uint8_t)(caps & 0xff);
    b[off++] = (uint8_t)((caps >> 8) & 0xff);
    b[off++] = (uint8_t)((caps >> 16) & 0xff);
    b[off++] = (uint8_t)((caps >> 24) & 0xff);
    return off;
}

static bool unpack_name(const uint8_t* b, int len, int off, char out[BUDDY_NAME_MAX + 1]) {
    if(off >= len) return false;
    uint8_t n = b[off++];
    if(n > BUDDY_NAME_MAX || off + n > len) return false;
    memcpy(out, &b[off], n);
    out[n] = '\0';
    return true;
}

/* ─────── pairing policy ─────── */

static bool pair_accept_allowed(void) {
#if BUDDY_AUTO_ACCEPT_PAIR
    return true;
#else
    /* Accept only while the pairing button is held to GND (active low). */
    gpio_set_direction(BUDDY_PAIR_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUDDY_PAIR_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    return gpio_get_level(BUDDY_PAIR_BUTTON_GPIO) == 0;
#endif
}

/* ─────── outbound builders ─────── */

static void send_discover_rsp(const uint8_t* dst) {
    uint8_t o[BUDDY_MAX_PAYLOAD];
    o[0] = BUDDY_MAGIC;
    o[1] = BuddyWireDiscoverRsp;
    uint8_t off = pack_name(o, 2, s_name);
    off = pack_caps(o, off, buddy_features_caps());
    buddy_espnow_send(dst, o, off);
}

static void send_pair_rsp(const uint8_t* dst, bool accepted) {
    uint8_t o[BUDDY_MAX_PAYLOAD];
    o[0] = BUDDY_MAGIC;
    o[1] = BuddyWirePairRsp;
    o[2] = accepted ? 1 : 0;
    uint8_t off = pack_name(o, 3, s_name);
    off = pack_caps(o, off, buddy_features_caps());
    buddy_espnow_send(dst, o, off);
}

static void send_feature_list(const uint8_t* dst) {
    size_t count = 0;
    const BuddyFeature* const* list = buddy_features_list(&count);

    uint8_t o[BUDDY_MAX_PAYLOAD];
    o[0] = BUDDY_MAGIC;
    o[1] = BuddyWireFeatureList;
    uint8_t off = 3; /* o[2] = entry count, filled in below */
    uint8_t n = 0;
    for(size_t i = 0; i < count; ++i) {
        size_t nl = strnlen(list[i]->name, BUDDY_NAME_MAX);
        /* +4 reserviert für die running_mask am Ende. */
        if((size_t)off + 1 + 1 + nl + 4 > BUDDY_MAX_PAYLOAD) break; /* frame full */
        o[off++] = list[i]->id;
        off = pack_name(o, off, list[i]->name);
        n++;
    }
    o[2] = n;
    /* running_mask anhängen: welche Features laufen GERADE (Quelle der Wahrheit
     * für den Master — er cached keinen Status). */
    off = pack_caps(o, off, buddy_features_running_mask());
    buddy_espnow_send(dst, o, off);
}

/* ─────── inbound handlers ─────── */

static void handle_pair_req(const uint8_t* src, const uint8_t* data, int len) {
    char mname[BUDDY_NAME_MAX + 1];
    if(!unpack_name(data, len, 2, mname)) mname[0] = '\0';

    bool accept = pair_accept_allowed();
    if(accept) {
        memcpy(s_master.mac, src, BUDDY_MAC_LEN);
        strlcpy(s_master.name, mname, sizeof(s_master.name));
        s_master.valid = true;
        buddy_store_save_master(src, mname);
        buddy_espnow_ensure_peer(src);
        ESP_LOGI(TAG, "paired with '%s'", mname);
    } else {
        ESP_LOGI(TAG, "pair request from '%s' rejected (button not held)", mname);
    }
    send_pair_rsp(src, accept);
}

static void handle_disconnect(const uint8_t* src) {
    uint8_t ack[2] = {BUDDY_MAGIC, BuddyWireDisconnectAck};
    buddy_espnow_send(src, ack, sizeof(ack));

    if(s_master.valid && memcmp(s_master.mac, src, BUDDY_MAC_LEN) == 0) {
        /* Unpaired → laufende Features stoppen (kein Master mehr zum Streamen). */
        buddy_features_stop_all();
        s_master.valid = false;
        buddy_store_clear_master();
        ESP_LOGI(TAG, "disconnected by master");
    }
}

static void handle_feature_start(const uint8_t* data, int len) {
    if(len < 4) return;
    uint8_t id = data[2];
    uint8_t arg_len = data[3];
    if(4 + arg_len > len) arg_len = (uint8_t)(len - 4); /* clamp to frame */

    const BuddyFeature* f = buddy_features_get(id);
    if(!f || !f->start) {
        ESP_LOGW(TAG, "start: unknown feature %u", id);
        buddy_node_feature_status(id, BuddyFeatStateError, NULL, 0);
        return;
    }
    ESP_LOGI(TAG, "start feature %u (%s)", id, f->name);
    f->start(&data[4], arg_len);
}

static void handle_feature_stop(const uint8_t* data, int len) {
    if(len < 3) return;
    uint8_t id = data[2];
    const BuddyFeature* f = buddy_features_get(id);
    if(!f || !f->stop) {
        ESP_LOGW(TAG, "stop: unknown feature %u", id);
        buddy_node_feature_status(id, BuddyFeatStateError, NULL, 0);
        return;
    }
    ESP_LOGI(TAG, "stop feature %u (%s)", id, f->name);
    f->stop();
}

/* ─────── public ─────── */

void buddy_node_on_frame(const uint8_t src[BUDDY_MAC_LEN], const uint8_t* data, int len, void* ctx) {
    (void)ctx;
    if(len < 2 || data[0] != BUDDY_MAGIC) return;

    switch(data[1]) {
    case BuddyWireDiscoverReq:
        ESP_LOGI(TAG, "rx DiscoverReq");
        send_discover_rsp(src);
        break;
    case BuddyWirePairReq:
        handle_pair_req(src, data, len);
        break;
    case BuddyWireDisconnect:
        handle_disconnect(src);
        break;
    case BuddyWireFeatureQuery:
        ESP_LOGI(TAG, "rx FeatureQuery");
        send_feature_list(src);
        break;
    case BuddyWireFeatureStart:
        handle_feature_start(data, len);
        break;
    case BuddyWireFeatureStop:
        handle_feature_stop(data, len);
        break;
    case BuddyWireResultAck:
        if(len >= 3) buddy_hs_store_ack(data[2]);
        break;
    default:
        /* Rsp/Status are buddy→master; nothing to do if echoed back. */
        break;
    }

    /* Any frame from the master means it is reachable right now — deliver any
     * pending handshakes (retained until acked, so this survives master absence). */
    if(s_master.valid && buddy_hs_store_has_pending()) {
        buddy_hs_store_pump(s_master.mac);
    }
}

void buddy_node_feature_status(uint8_t feat_id, uint8_t state, const uint8_t* data, uint8_t len) {
    if(!s_master.valid) return;
    if(len > BUDDY_MAX_PAYLOAD - 5) len = BUDDY_MAX_PAYLOAD - 5;

    uint8_t o[BUDDY_MAX_PAYLOAD];
    o[0] = BUDDY_MAGIC;
    o[1] = BuddyWireFeatureStatus;
    o[2] = feat_id;
    o[3] = state;
    o[4] = len;
    if(data && len) memcpy(&o[5], data, len);
    buddy_espnow_send(s_master.mac, o, 5 + len);
}

const char* buddy_node_self_name(void) {
    return s_name;
}

bool buddy_node_get_master_mac(uint8_t out[BUDDY_MAC_LEN]) {
    if(!s_master.valid) return false;
    memcpy(out, s_master.mac, BUDDY_MAC_LEN);
    return true;
}

void buddy_node_init(void) {
    buddy_hs_store_init();

    uint8_t mac[BUDDY_MAC_LEN] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(
        s_name, sizeof(s_name), "%s-%02X%02X%02X", BUDDY_NAME_PREFIX, mac[3], mac[4], mac[5]);

    s_master.valid = false;
    if(buddy_store_load_master(&s_master)) {
        ESP_LOGI(
            TAG,
            "stored master '%s' %02X:%02X:%02X:%02X:%02X:%02X",
            s_master.name,
            s_master.mac[0],
            s_master.mac[1],
            s_master.mac[2],
            s_master.mac[3],
            s_master.mac[4],
            s_master.mac[5]);
    }
}

void buddy_node_ready(void) {
    if(s_master.valid) buddy_espnow_ensure_peer(s_master.mac);
    ESP_LOGI(
        TAG,
        "buddy '%s' ready, caps=0x%08X, %s",
        s_name,
        (unsigned)buddy_features_caps(),
        s_master.valid ? "paired" : "unpaired");
}

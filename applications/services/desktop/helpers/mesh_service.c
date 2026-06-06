#include "mesh_service.h"

#include <furi.h>
#include <furi_hal.h>

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_mac.h>

#define TAG "MeshSvc"

#define MESH_CHANNEL    1
#define MESH_MAGIC      0x4D /* 'M' */
#define MESH_MAX_PAYLOAD 64

/* Wire-Format:
 *   [0] magic = 0x4D
 *   [1] type
 *   [2..]  type-abhängig
 *
 * payload-Encoding für name-tragende Typen:
 *   [name_len:1][name:name_len]    (name_len <= 32)
 *
 * Spezielles Encoding für PAIR_RSP:
 *   [accepted:1][name_len:1][name:name_len]
 */
typedef enum {
    MeshWireDiscoverReq   = 1,
    MeshWireDiscoverRsp   = 2,  // + caps hinter dem Namen
    MeshWirePairReq       = 3,
    MeshWirePairRsp       = 4,  // + caps hinter dem Namen
    MeshWireDisconnect    = 5,
    MeshWireDisconnectAck = 6,
    /* Feature-Steuerung (Erweiterung, vom Buddy implementiert). */
    MeshWireFeatureQuery  = 7,  // master → buddy : (kein payload)
    MeshWireFeatureList   = 8,  // buddy  → master: [count][ {id, name} ... ]
    MeshWireFeatureStart  = 9,  // master → buddy : [id][arg_len][args]
    MeshWireFeatureStop   = 10, // master → buddy : [id]
    MeshWireFeatureStatus = 11, // buddy  → master: [id][state][len][data]
    MeshWirePcapFrame     = 12, // buddy  → master: [seq][frag_idx][frag_cnt][data]
    MeshWireResult        = 13, // buddy  → master: [id][type][len][data]  (zuverlässig, ack)
    MeshWireResultAck     = 14, // master → buddy : [id]
} MeshWireType;

#define MESH_PCAP_HDR 5 /* magic,type,seq,frag_idx,frag_cnt */

/* ─────── command queue ─────── */

typedef enum {
    MeshCmdStop = 0,
    MeshCmdSend,
    MeshCmdSetChannel,
} MeshCmdKind;

typedef struct {
    MeshCmdKind kind;
    uint8_t mac[MESH_MAC_LEN]; // 0xff..ff = broadcast
    uint8_t buf[MESH_MAX_PAYLOAD];
    uint8_t len;
    uint8_t channel; // für MeshCmdSetChannel
} MeshCmd;

#define MESH_CMD_QLEN 16

/* ─────── service state ─────── */

static struct {
    MeshRole role;
    MeshEventCallback cb;
    void* cb_ctx;

    bool active;

    TaskHandle_t worker;
    QueueHandle_t cmd_q;
    SemaphoreHandle_t ready_sem;
    SemaphoreHandle_t done_sem;

    uint8_t self_mac[MESH_MAC_LEN];
} s_svc;

static const uint8_t BROADCAST_MAC[MESH_MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* ─────── pcap-Streaming (Frame-Reassembly aus BuddyWirePcapFrame) ─────── */
#define MESH_PCAP_FRAME_MAX 512

static MeshPcapSink s_pcap_sink = NULL;
static void* s_pcap_sink_ctx = NULL;
static struct {
    uint8_t buf[MESH_PCAP_FRAME_MAX];
    uint16_t len;
    uint8_t seq;
    uint8_t next_frag; /* erwarteter frag_idx */
    bool active;
} s_pcap_asm;

void mesh_set_pcap_sink(MeshPcapSink sink, void* ctx) {
    s_pcap_sink = sink;
    s_pcap_sink_ctx = ctx;
    s_pcap_asm.active = false;
}

/* ─────── helpers ─────── */

static const char* mesh_self_name(void) {
    const char* n = furi_hal_version_get_name_ptr();
    return n ? n : "esp32";
}

/* Stellt sicher dass ein Peer registriert ist (esp_now_add_peer ist nicht
 * idempotent — Duplicate gibt EXIST zurück). */
static void mesh_ensure_peer(const uint8_t mac[MESH_MAC_LEN]) {
    if(esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t pi = {0};
    memcpy(pi.peer_addr, mac, MESH_MAC_LEN);
    pi.channel = 0; /* 0 = aktueller Radio-Kanal — wir folgen dem Buddy beim Capture */
    pi.ifidx = WIFI_IF_STA;
    pi.encrypt = false;
    esp_err_t err = esp_now_add_peer(&pi);
    if(err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        FURI_LOG_W(TAG, "add_peer: %s", esp_err_to_name(err));
    }
}

/* Baut [name_len][name] in buf ab off; return neue Länge. */
static uint8_t pack_name(uint8_t* buf, uint8_t off, const char* name) {
    size_t n = strnlen(name, MESH_NAME_MAX);
    buf[off++] = (uint8_t)n;
    memcpy(&buf[off], name, n);
    return off + (uint8_t)n;
}

static bool unpack_name(const uint8_t* buf, int len, int off, char out[MESH_NAME_MAX + 1]) {
    if(off >= len) return false;
    uint8_t n = buf[off++];
    if(n > MESH_NAME_MAX || off + n > len) return false;
    memcpy(out, &buf[off], n);
    out[n] = '\0';
    return true;
}

/* Offset direkt hinter [name_len][name] ab off; -1 bei Out-of-bounds. */
static int name_end_off(const uint8_t* buf, int len, int off) {
    if(off >= len) return -1;
    uint8_t n = buf[off];
    if(off + 1 + n > len) return -1;
    return off + 1 + (int)n;
}

/* 4-Byte little-endian caps ab off; 0 wenn nicht genug Bytes (alter Client). */
static uint32_t unpack_caps(const uint8_t* buf, int len, int off) {
    if(off < 0 || off + 4 > len) return 0;
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) | ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

static bool mesh_enqueue(const uint8_t mac[MESH_MAC_LEN], const uint8_t* data, uint8_t len) {
    if(!s_svc.active || !s_svc.cmd_q) return false;
    if(len > MESH_MAX_PAYLOAD) return false;
    MeshCmd c = {.kind = MeshCmdSend, .len = len};
    memcpy(c.mac, mac, MESH_MAC_LEN);
    memcpy(c.buf, data, len);
    return xQueueSend(s_svc.cmd_q, &c, pdMS_TO_TICKS(50)) == pdTRUE;
}

/* ─────── ESP-NOW callbacks (run in WiFi-internal task context) ─────── */

static void on_send_cb(const uint8_t* mac, esp_now_send_status_t status) {
    (void)mac;
    if(status != ESP_NOW_SEND_SUCCESS) {
        FURI_LOG_D(TAG, "send fail");
    }
}

static void on_recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if(!info || !data || len < 2) return;
    if(data[0] != MESH_MAGIC) return;

    const uint8_t type = data[1];
    const uint8_t* src = info->src_addr;
    const uint8_t rxch = info->rx_ctrl ? (uint8_t)info->rx_ctrl->channel : 0;

    /* Auto-Reply-Pfad und User-Callback-Dispatch nach Type. */
    switch(type) {
    case MeshWireDiscoverReq: {
        if(s_svc.role != MeshRoleClient) return;
        /* Antwort: DISCOVER_RSP mit eigenem Namen. */
        uint8_t out[MESH_MAX_PAYLOAD];
        out[0] = MESH_MAGIC;
        out[1] = MeshWireDiscoverRsp;
        uint8_t n = pack_name(out, 2, mesh_self_name());
        mesh_ensure_peer(src);
        mesh_enqueue(src, out, n);
        /* kein User-Callback */
        return;
    }
    case MeshWireDiscoverRsp: {
        if(s_svc.role != MeshRoleMaster) return;
        MeshEventData ev = {.type = MeshEventDiscoverResponse};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.rx_channel = rxch;
        if(!unpack_name(data, len, 2, ev.name)) return;
        ev.caps = unpack_caps(data, len, name_end_off(data, len, 2));
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWirePairReq: {
        if(s_svc.role != MeshRoleClient) return;
        MeshEventData ev = {.type = MeshEventPairRequest};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        if(!unpack_name(data, len, 2, ev.name)) return;
        mesh_ensure_peer(src);
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWirePairRsp: {
        if(s_svc.role != MeshRoleMaster) return;
        if(len < 3) return;
        MeshEventData ev = {
            .type = MeshEventPairResponse,
            .accepted = data[2] ? true : false,
        };
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.rx_channel = rxch;
        if(!unpack_name(data, len, 3, ev.name)) return;
        ev.caps = unpack_caps(data, len, name_end_off(data, len, 3));
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWireDisconnect: {
        if(s_svc.role != MeshRoleClient) return;
        /* Silent ACK senden. */
        uint8_t out[2] = {MESH_MAGIC, MeshWireDisconnectAck};
        mesh_ensure_peer(src);
        mesh_enqueue(src, out, sizeof(out));
        /* UI informieren damit master.txt gelöscht werden kann. */
        MeshEventData ev = {.type = MeshEventDisconnect};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.name[0] = '\0';
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWireDisconnectAck:
        /* Nichts zu tun — Master hatte den Peer schon entfernt. */
        return;
    case MeshWireFeatureList: {
        if(s_svc.role != MeshRoleMaster) return;
        if(len < 3) return;
        MeshEventData ev = {.type = MeshEventFeatureList};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.rx_channel = rxch;
        uint8_t count = data[2];
        int off = 3;
        uint8_t n = 0;
        for(uint8_t i = 0; i < count && n < MESH_FEATURES_MAX; ++i) {
            if(off >= len) break;
            uint8_t id = data[off++];
            char nm[MESH_NAME_MAX + 1];
            if(!unpack_name(data, len, off, nm)) break;
            off = name_end_off(data, len, off);
            if(off < 0) break;
            ev.features[n].id = id;
            strncpy(ev.features[n].name, nm, MESH_FEAT_NAME_MAX);
            ev.features[n].name[MESH_FEAT_NAME_MAX] = '\0';
            n++;
        }
        ev.feature_count = n;
        /* running_mask folgt hinter den Einträgen (0 bei altem Buddy). */
        ev.running_mask = (off >= 0) ? unpack_caps(data, len, off) : 0;
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWireFeatureStatus: {
        if(s_svc.role != MeshRoleMaster) return;
        if(len < 5) return;
        MeshEventData ev = {.type = MeshEventFeatureStatus};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.rx_channel = rxch;
        ev.feat_id = data[2];
        ev.feat_state = data[3];
        uint8_t dlen = data[4];
        if(dlen > MESH_FEAT_DATA_MAX) dlen = MESH_FEAT_DATA_MAX;
        if(5 + dlen > len) dlen = (uint8_t)(len - 5);
        memcpy(ev.feat_data, &data[5], dlen);
        ev.feat_data[dlen] = '\0';
        ev.feat_data_len = dlen;
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    case MeshWirePcapFrame: {
        if(s_svc.role != MeshRoleMaster || !s_pcap_sink) return;
        if(len < MESH_PCAP_HDR) return;
        uint8_t seq = data[2];
        uint8_t frag_idx = data[3];
        uint8_t frag_cnt = data[4];
        const uint8_t* chunk = &data[MESH_PCAP_HDR];
        int chunk_len = len - MESH_PCAP_HDR;
        if(frag_cnt == 0) return;

        if(frag_idx == 0) {
            /* neuer Frame */
            s_pcap_asm.active = true;
            s_pcap_asm.seq = seq;
            s_pcap_asm.next_frag = 0;
            s_pcap_asm.len = 0;
        }
        /* Fragment muss zur laufenden Reassembly passen, sonst verwerfen. */
        if(!s_pcap_asm.active || s_pcap_asm.seq != seq || s_pcap_asm.next_frag != frag_idx) {
            s_pcap_asm.active = false;
            return;
        }
        if(s_pcap_asm.len + chunk_len > MESH_PCAP_FRAME_MAX) {
            s_pcap_asm.active = false;
            return;
        }
        memcpy(&s_pcap_asm.buf[s_pcap_asm.len], chunk, chunk_len);
        s_pcap_asm.len += (uint16_t)chunk_len;
        s_pcap_asm.next_frag++;

        if(s_pcap_asm.next_frag >= frag_cnt) {
            s_pcap_asm.active = false;
            if(s_pcap_sink) s_pcap_sink(s_pcap_asm.buf, s_pcap_asm.len, s_pcap_sink_ctx);
        }
        return;
    }
    case MeshWireResult: {
        if(s_svc.role != MeshRoleMaster) return;
        if(len < 5) return; /* magic,type,id,type,len */
        MeshEventData ev = {.type = MeshEventResult};
        memcpy(ev.mac, src, MESH_MAC_LEN);
        ev.rx_channel = rxch;
        ev.result_id = data[2];
        ev.result_type = data[3];
        uint8_t dlen = data[4];
        if(dlen > MESH_FEAT_DATA_MAX) dlen = MESH_FEAT_DATA_MAX;
        if(5 + dlen > len) dlen = (uint8_t)(len - 5);
        memcpy(ev.feat_data, &data[5], dlen);
        ev.feat_data[dlen] = '\0';
        ev.feat_data_len = dlen;
        if(s_svc.cb) s_svc.cb(&ev, s_svc.cb_ctx);
        return;
    }
    default:
        return;
    }
}

/* ─────── Worker-Task (echter FreeRTOS-Task wegen esp_wifi_*) ─────── */

static bool s_netif_initialized = false;

static bool wifi_init_once(void) {
    esp_err_t err = esp_netif_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FURI_LOG_E(TAG, "netif_init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        FURI_LOG_E(TAG, "event_loop: %s", esp_err_to_name(err));
        return false;
    }
    if(!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }
    s_netif_initialized = true;
    return true;
}

static void worker_task(void* arg) {
    (void)arg;

    if(!wifi_init_once()) goto fail;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Sehr knappes Buffer-Profil — kein TCP-Traffic, nur ESP-NOW frames. */
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 4;
    cfg.dynamic_tx_buf_num = 4;

    esp_err_t err = esp_wifi_init(&cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "wifi_init: %s", esp_err_to_name(err));
        goto fail;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        goto fail;
    }
    esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, s_svc.self_mac);

    if(esp_now_init() != ESP_OK) {
        FURI_LOG_E(TAG, "esp_now_init failed");
        esp_wifi_stop();
        esp_wifi_deinit();
        goto fail;
    }
    esp_now_register_recv_cb(on_recv_cb);
    esp_now_register_send_cb(on_send_cb);

    /* Broadcast-Peer für Discovery. */
    mesh_ensure_peer(BROADCAST_MAC);

    /* Bekannte Peers für Client (Master) / Master (alle bekannten Clients)
     * registrieren — spart Latenz beim ersten Send. */
    if(s_svc.role == MeshRoleClient) {
        MeshPeer m;
        if(mesh_config_load_master(&m)) mesh_ensure_peer(m.mac);
    } else if(s_svc.role == MeshRoleMaster) {
        MeshPeer list[MESH_CLIENTS_MAX];
        size_t n = 0;
        mesh_config_load_clients(list, &n);
        for(size_t i = 0; i < n; ++i) mesh_ensure_peer(list[i].mac);
    }

    FURI_LOG_I(
        TAG,
        "ready role=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
        s_svc.role,
        s_svc.self_mac[0],
        s_svc.self_mac[1],
        s_svc.self_mac[2],
        s_svc.self_mac[3],
        s_svc.self_mac[4],
        s_svc.self_mac[5]);

    xSemaphoreGive(s_svc.ready_sem);

    /* Cmd-Loop. */
    MeshCmd c;
    for(;;) {
        if(xQueueReceive(s_svc.cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        if(c.kind == MeshCmdStop) break;
        if(c.kind == MeshCmdSend) {
            mesh_ensure_peer(c.mac);
            esp_err_t e = esp_now_send(c.mac, c.buf, c.len);
            if(e != ESP_OK) FURI_LOG_W(TAG, "esp_now_send: %s", esp_err_to_name(e));
        } else if(c.kind == MeshCmdSetChannel) {
            /* Kurz warten, damit ein direkt davor gequeuetes Send (z.B.
             * FeatureStart/Stop) noch auf dem alten Kanal rausgeht, bevor wir
             * den Radio-Kanal umstellen (dem Buddy hinterher beim Capture). */
            vTaskDelay(pdMS_TO_TICKS(60));
            esp_err_t ce = esp_wifi_set_channel(c.channel, WIFI_SECOND_CHAN_NONE);
            FURI_LOG_I(TAG, "channel -> %u (%s)", c.channel, esp_err_to_name(ce));
        }
    }

    /* Teardown. */
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    xSemaphoreGive(s_svc.done_sem);
    s_svc.worker = NULL;
    vTaskDelete(NULL);
    return;

fail:
    xSemaphoreGive(s_svc.ready_sem);  // unblockt start() — active bleibt false
    xSemaphoreGive(s_svc.done_sem);
    s_svc.worker = NULL;
    vTaskDelete(NULL);
}

/* ─────── public API ─────── */

bool mesh_service_start(MeshRole role, MeshEventCallback cb, void* ctx) {
    if(s_svc.active) {
        /* Re-configure callback wenn schon aktiv und Rolle identisch. */
        if(s_svc.role == role) {
            s_svc.cb = cb;
            s_svc.cb_ctx = ctx;
            return true;
        }
        mesh_service_stop();
    }
    if(role == MeshRoleNone) return false;

    s_svc.role = role;
    s_svc.cb = cb;
    s_svc.cb_ctx = ctx;
    s_svc.cmd_q = xQueueCreate(MESH_CMD_QLEN, sizeof(MeshCmd));
    s_svc.ready_sem = xSemaphoreCreateBinary();
    s_svc.done_sem = xSemaphoreCreateBinary();
    s_svc.active = true;

    BaseType_t ok = xTaskCreate(worker_task, "mesh_svc", 4096, NULL, 5, &s_svc.worker);
    if(ok != pdPASS) {
        FURI_LOG_E(TAG, "xTaskCreate failed");
        goto fail;
    }
    /* Auf Ready warten. */
    if(xSemaphoreTake(s_svc.ready_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        FURI_LOG_E(TAG, "start timeout");
        goto fail;
    }
    /* Wenn worker_task fail-Pfad genommen hat ist worker bereits NULL. */
    if(s_svc.worker == NULL) {
        FURI_LOG_E(TAG, "worker init failed");
        goto fail;
    }
    return true;

fail:
    s_svc.active = false;
    if(s_svc.cmd_q) { vQueueDelete(s_svc.cmd_q); s_svc.cmd_q = NULL; }
    if(s_svc.ready_sem) { vSemaphoreDelete(s_svc.ready_sem); s_svc.ready_sem = NULL; }
    if(s_svc.done_sem) { vSemaphoreDelete(s_svc.done_sem); s_svc.done_sem = NULL; }
    s_svc.role = MeshRoleNone;
    s_svc.cb = NULL;
    s_svc.cb_ctx = NULL;
    return false;
}

void mesh_service_stop(void) {
    if(!s_svc.active) return;
    s_svc.active = false;

    MeshCmd stop = {.kind = MeshCmdStop};
    xQueueSend(s_svc.cmd_q, &stop, portMAX_DELAY);

    /* Auf Teardown warten (großzügig — wifi_deinit kann dauern). */
    xSemaphoreTake(s_svc.done_sem, pdMS_TO_TICKS(3000));

    vQueueDelete(s_svc.cmd_q); s_svc.cmd_q = NULL;
    vSemaphoreDelete(s_svc.ready_sem); s_svc.ready_sem = NULL;
    vSemaphoreDelete(s_svc.done_sem); s_svc.done_sem = NULL;
    s_svc.role = MeshRoleNone;
    s_svc.cb = NULL;
    s_svc.cb_ctx = NULL;
    memset(s_svc.self_mac, 0, MESH_MAC_LEN);
}

bool mesh_service_is_active(void) { return s_svc.active; }
MeshRole mesh_service_get_role(void) { return s_svc.role; }

bool mesh_service_get_self_mac(uint8_t out[MESH_MAC_LEN]) {
    if(!s_svc.active) return false;
    memcpy(out, s_svc.self_mac, MESH_MAC_LEN);
    return true;
}

/* ─────── Sender (alle queueing-basiert) ─────── */

bool mesh_send_discover(void) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[MESH_MAX_PAYLOAD];
    out[0] = MESH_MAGIC;
    out[1] = MeshWireDiscoverReq;
    uint8_t n = pack_name(out, 2, mesh_self_name());
    return mesh_enqueue(BROADCAST_MAC, out, n);
}

bool mesh_send_pair_request(const uint8_t to[MESH_MAC_LEN]) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[MESH_MAX_PAYLOAD];
    out[0] = MESH_MAGIC;
    out[1] = MeshWirePairReq;
    uint8_t n = pack_name(out, 2, mesh_self_name());
    return mesh_enqueue(to, out, n);
}

bool mesh_send_pair_response(const uint8_t to[MESH_MAC_LEN], bool accepted) {
    if(s_svc.role != MeshRoleClient) return false;
    uint8_t out[MESH_MAX_PAYLOAD];
    out[0] = MESH_MAGIC;
    out[1] = MeshWirePairRsp;
    out[2] = accepted ? 1 : 0;
    uint8_t n = pack_name(out, 3, mesh_self_name());
    return mesh_enqueue(to, out, n);
}

bool mesh_send_disconnect(const uint8_t to[MESH_MAC_LEN]) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[2] = {MESH_MAGIC, MeshWireDisconnect};
    return mesh_enqueue(to, out, sizeof(out));
}

bool mesh_send_feature_query(const uint8_t to[MESH_MAC_LEN]) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[2] = {MESH_MAGIC, MeshWireFeatureQuery};
    return mesh_enqueue(to, out, sizeof(out));
}

bool mesh_send_feature_start(
    const uint8_t to[MESH_MAC_LEN],
    uint8_t feat_id,
    const uint8_t* args,
    uint8_t arg_len) {
    if(s_svc.role != MeshRoleMaster) return false;
    if((int)arg_len > MESH_MAX_PAYLOAD - 4) arg_len = MESH_MAX_PAYLOAD - 4;
    uint8_t out[MESH_MAX_PAYLOAD];
    out[0] = MESH_MAGIC;
    out[1] = MeshWireFeatureStart;
    out[2] = feat_id;
    out[3] = arg_len;
    if(args && arg_len) memcpy(&out[4], args, arg_len);
    return mesh_enqueue(to, out, 4 + arg_len);
}

bool mesh_send_feature_stop(const uint8_t to[MESH_MAC_LEN], uint8_t feat_id) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[3] = {MESH_MAGIC, MeshWireFeatureStop, feat_id};
    return mesh_enqueue(to, out, sizeof(out));
}

bool mesh_send_result_ack(const uint8_t to[MESH_MAC_LEN], uint8_t result_id) {
    if(s_svc.role != MeshRoleMaster) return false;
    uint8_t out[3] = {MESH_MAGIC, MeshWireResultAck, result_id};
    return mesh_enqueue(to, out, sizeof(out));
}

void mesh_service_set_channel(uint8_t channel) {
    if(!s_svc.active || !s_svc.cmd_q) return;
    MeshCmd c = {.kind = MeshCmdSetChannel, .channel = channel};
    xQueueSend(s_svc.cmd_q, &c, pdMS_TO_TICKS(50));
}

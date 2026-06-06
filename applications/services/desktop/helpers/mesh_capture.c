#include "mesh_capture.h"
#include "mesh_service.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "MeshCapture"

#define CAP_FRAME_MAX 512 /* pcap snaplen, matches buddy CAP_PKT_MAX */
#define CAP_RING      24
#define CAP_DIR       "/ext/wifi"

#define STOP_RETRY_MS   400
#define STOP_TIMEOUT_MS 15000

/* pcap (libpcap classic) — selbe Header wie wlan_pcap.c. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network; /* 105 = LINKTYPE_IEEE802_11 */
} PcapGlobalHeader;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} PcapPacketHeader;

typedef struct {
    uint16_t len;
    uint8_t data[CAP_FRAME_MAX];
} CapFrame;

typedef enum {
    CapIdle = 0,
    CapRunning,
    CapStopping,
} CapState;

static struct {
    volatile CapState state;
    uint8_t mac[MESH_MAC_LEN];
    uint8_t feat_id;
    char path[200];

    Storage* storage;
    File* file;

    CapFrame* ring;
    volatile uint32_t wr;
    volatile uint32_t rd;

    FuriThread* writer;
    volatile bool exit_req;
    volatile bool stopped_ack;
    uint32_t stop_started_tick;

    uint32_t frame_count;
} s;

/* ─────── pcap io ─────── */
static File* pcap_open(Storage* storage, const char* path) {
    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(f);
        return NULL;
    }
    PcapGlobalHeader h = {
        .magic = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = CAP_FRAME_MAX,
        .network = 105,
    };
    storage_file_write(f, &h, sizeof(h));
    return f;
}

static void pcap_write(File* f, uint32_t ts_us, const uint8_t* data, uint16_t len) {
    if(!f || !data || len == 0) return;
    PcapPacketHeader ph = {
        .ts_sec = ts_us / 1000000,
        .ts_usec = ts_us % 1000000,
        .incl_len = len,
        .orig_len = len,
    };
    storage_file_write(f, &ph, sizeof(ph));
    storage_file_write(f, data, len);
}

/* ─────── sink (WiFi-Task): Frame in den Ring kopieren ─────── */
static void capture_sink(const uint8_t* frame, uint16_t len, void* ctx) {
    (void)ctx;
    if(!s.ring || len == 0 || len > CAP_FRAME_MAX) return;
    uint32_t next = (s.wr + 1) % CAP_RING;
    if(next == s.rd) return; /* voll — droppen */
    CapFrame* slot = &s.ring[s.wr];
    memcpy(slot->data, frame, len);
    slot->len = len;
    s.wr = next;
}

static void drain_ring(void) {
    while(s.rd != s.wr) {
        CapFrame* fr = &s.ring[s.rd];
        uint32_t ts_us = furi_get_tick() * 1000u;
        pcap_write(s.file, ts_us, fr->data, fr->len);
        s.frame_count++;
        s.rd = (s.rd + 1) % CAP_RING;
    }
}

/* Räumt alle Capture-Ressourcen auf (außer dem Thread-Handle selbst). Wird nur
 * aus dem Writer-Thread aufgerufen; alle Schritte sind NULL-geschützt. */
static void finalize_resources(void) {
    mesh_set_pcap_sink(NULL, NULL); /* keine neuen Frames mehr in den Ring */
    drain_ring(); /* finaler Rest */
    if(s.file) {
        storage_file_close(s.file);
        storage_file_free(s.file);
        s.file = NULL;
    }
    if(s.ring) {
        free(s.ring);
        s.ring = NULL;
    }
    if(s.storage) {
        furi_record_close(RECORD_STORAGE);
        s.storage = NULL;
    }
    /* Master-Radio zurück auf ch1 (Discovery-Sweep startet dort). */
    mesh_service_set_channel(1);
}

/* ─────── writer thread: drain + stop-retry, dann Selbst-Finalisierung ─────── */
static int32_t writer_fn(void* arg) {
    (void)arg;
    uint32_t last_retry = 0;
    while(!s.exit_req) {
        drain_ring();

        if(s.state == CapStopping) {
            uint32_t now = furi_get_tick();
            if(now - last_retry >= furi_ms_to_ticks(STOP_RETRY_MS)) {
                mesh_send_feature_stop(s.mac, s.feat_id);
                last_retry = now;
            }
            if(s.stopped_ack || (now - s.stop_started_tick) >= furi_ms_to_ticks(STOP_TIMEOUT_MS)) {
                break;
            }
        }
        furi_delay_ms(80);
    }

    finalize_resources();
    FURI_LOG_I(TAG, "capture finalized: %lu frames -> %s", (unsigned long)s.frame_count, s.path);
    s.state = CapIdle; /* Liste zeigt Client wieder als Idle; Handle wird gereaped */
    return 0;
}

/* ─────── public ─────── */
static void sanitize(const char* in, char* out, size_t out_len) {
    size_t j = 0;
    for(size_t i = 0; in && in[i] && j < out_len - 1; ++i) {
        char c = in[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    if(j == 0) out[j++] = 'b';
    out[j] = '\0';
}

/* Gemeinsames Setup: pcap öffnen, Ring + Sink + Writer-Thread starten. Sendet
 * KEIN FeatureStart und wechselt KEINEN Kanal (Master bleibt auf ch1; der Buddy
 * besucht ch1 selbst, um zu streamen). */
static bool open_session(const uint8_t mac[MESH_MAC_LEN], const char* client_name, uint8_t feat_id) {
    if(s.state != CapIdle) return false;

    /* Frühere, selbst-finalisierte Session einsammeln (join kehrt sofort zurück). */
    if(s.writer) {
        furi_thread_join(s.writer);
        furi_thread_free(s.writer);
        s.writer = NULL;
    }

    s.storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(s.storage, CAP_DIR);

    char safe[40];
    sanitize(client_name, safe, sizeof(safe));
    snprintf(s.path, sizeof(s.path), "%s/buddy_%s.pcap", CAP_DIR, safe);
    if(storage_common_stat(s.storage, s.path, NULL) == FSE_OK) {
        for(int i = 1; i < 999; ++i) {
            snprintf(s.path, sizeof(s.path), "%s/buddy_%s_%d.pcap", CAP_DIR, safe, i);
            if(storage_common_stat(s.storage, s.path, NULL) != FSE_OK) break;
        }
    }

    s.file = pcap_open(s.storage, s.path);
    if(!s.file) {
        FURI_LOG_E(TAG, "pcap open failed: %s", s.path);
        furi_record_close(RECORD_STORAGE);
        s.storage = NULL;
        return false;
    }

    s.ring = malloc(sizeof(CapFrame) * CAP_RING);
    if(!s.ring) {
        storage_file_close(s.file);
        storage_file_free(s.file);
        s.file = NULL;
        furi_record_close(RECORD_STORAGE);
        s.storage = NULL;
        return false;
    }
    s.wr = s.rd = 0;
    s.frame_count = 0;
    s.exit_req = false;
    s.stopped_ack = false;
    memcpy(s.mac, mac, MESH_MAC_LEN);
    s.feat_id = feat_id;
    s.state = CapRunning;

    mesh_set_pcap_sink(capture_sink, NULL);

    s.writer = furi_thread_alloc();
    furi_thread_set_name(s.writer, "MeshCapWr");
    furi_thread_set_stack_size(s.writer, 2048);
    furi_thread_set_callback(s.writer, writer_fn);
    furi_thread_start(s.writer);
    return true;
}

bool mesh_capture_start(
    const uint8_t mac[MESH_MAC_LEN],
    const char* client_name,
    uint8_t feat_id,
    uint8_t channel) {
    if(!open_session(mac, client_name, feat_id)) return false;
    uint8_t arg = channel;
    mesh_send_feature_start(mac, feat_id, &arg, 1); /* Buddy capturet auf ch N (bleibt dort) */
    /* Dem Buddy auf seinen Capture-Kanal folgen — Promiscuous + ESP-NOW laufen
     * dort parallel, also Echtzeit-Stream + Kommandos auf demselben Kanal. */
    if(channel >= 1 && channel <= 13) mesh_service_set_channel(channel);
    FURI_LOG_I(TAG, "capture started ch=%u -> %s", channel, s.path);
    return true;
}

bool mesh_capture_attach(
    const uint8_t mac[MESH_MAC_LEN],
    const char* client_name,
    uint8_t feat_id,
    uint8_t channel) {
    /* Sammle die Ergebnisse eines bereits laufenden Buddy-Captures ein (z.B. nach
     * Master-Reboot) — KEIN FeatureStart (Buddy läuft schon), aber auf dessen
     * Kanal tunen (per Discovery-Sweep gefunden). */
    if(!open_session(mac, client_name, feat_id)) return false;
    if(channel >= 1 && channel <= 13) mesh_service_set_channel(channel);
    FURI_LOG_I(TAG, "capture attached ch=%u -> %s", channel, s.path);
    return true;
}

void mesh_capture_stop(void) {
    if(s.state != CapRunning) return;
    s.state = CapStopping;
    s.stop_started_tick = furi_get_tick();
    mesh_send_feature_stop(s.mac, s.feat_id);
}

void mesh_capture_finish(void) {
    /* Nur die LOKALE Sammel-Session beenden (pcap schließen) — der Buddy läuft
     * autonom weiter und wird NICHT gestoppt. Gestoppt wird er ausschließlich per
     * mesh_capture_stop() (User klickt Stop) oder Disconnect. So überlebt der
     * Capture ein Verlassen des Mesh-Bereichs / einen Master-Reboot. */

    /* Writer-Thread beenden (er ruft finalize_resources + setzt state=Idle) und
     * sein Handle freigeben. Idempotent — auch wenn er sich schon selbst beendet
     * hat (join kehrt sofort zurück). */
    s.exit_req = true;
    if(s.writer) {
        furi_thread_join(s.writer);
        furi_thread_free(s.writer);
        s.writer = NULL;
    }
    s.state = CapIdle;
    s.path[0] = '\0';
}

bool mesh_capture_is_active(void) {
    return s.state != CapIdle;
}

bool mesh_capture_get_mac(uint8_t out[MESH_MAC_LEN]) {
    if(s.state == CapIdle) return false;
    memcpy(out, s.mac, MESH_MAC_LEN);
    return true;
}

uint32_t mesh_capture_frame_count(void) {
    return s.frame_count;
}

const char* mesh_capture_path(void) {
    return s.state == CapIdle ? "" : s.path;
}

void mesh_capture_note_status(const uint8_t mac[MESH_MAC_LEN], uint8_t feat_id, uint8_t state) {
    if(s.state == CapIdle) return;
    if(feat_id != s.feat_id || memcmp(mac, s.mac, MESH_MAC_LEN) != 0) return;
    if(state == MeshFeatStateStopped) {
        s.stopped_ack = true; /* writer-Thread finalisiert beim nächsten Tick */
    }
}

#include "wlan_pcap_rec.h"
#include "wlan_pcap.h" // Low-Level pcap-Primitive (open/write_packet/close)

#include <furi.h>
#include <storage/storage.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_attr.h>

#define TAG "WlanPcapRec"

#define WLAN_PCAP_REC_SNAPLEN 512u
// Ringpuffer im PSRAM. Absorbiert Bursts, während die SD schreibt. 128 KB ist im
// 8-MB-PSRAM vernachlässigbar; der Control-Block (Semaphore) bleibt im DRAM.
#define WLAN_PCAP_REC_RB_SIZE (128u * 1024u)

// Ring-Item: [uint32 ts_us][frame bytes]. ts_us = us seit Aufzeichnungsstart.
#define WLAN_PCAP_REC_TS_LEN 4u

static volatile bool s_active;
static volatile bool s_run;

static RingbufHandle_t s_rb;
static StaticRingbuffer_t s_rb_struct; // Control-Struct im internen DRAM
static uint8_t* s_rb_storage;          // Daten-Arena im PSRAM

static Storage* s_storage;
static File* s_file;
static FuriThread* s_writer;

static int64_t s_base_us; // esp_timer-Basis (us) bei Start

static volatile uint32_t s_frames;
static volatile uint32_t s_bytes;
static volatile uint32_t s_drops;

static char s_path[112];

// Staging-Puffer für ein Ring-Item. Nur der WiFi-Task (Promiscuous-Callback)
// schreibt hier — Single-Producer, daher static unbedenklich.
static EXT_RAM_BSS_ATTR uint8_t s_stage[WLAN_PCAP_REC_TS_LEN + WLAN_PCAP_REC_SNAPLEN];

// Elternverzeichnis des Dateipfads rekursiv anlegen. wlan_pcap_open() legt nur
// "/ext/wifi" an, nicht tiefere Ebenen wie "/ext/wifi/sniffer".
static void wlan_pcap_rec_mkdirs(Storage* storage, const char* file_path) {
    char dir[128];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* last = strrchr(dir, '/');
    if(!last || last == dir) return;
    *last = 0; // dir = Elternverzeichnis
    for(char* p = dir + 1; *p; ++p) {
        if(*p == '/') {
            *p = 0;
            storage_simply_mkdir(storage, dir);
            *p = '/';
        }
    }
    storage_simply_mkdir(storage, dir);
}

static void wlan_pcap_rec_write_item(const uint8_t* item, size_t item_size) {
    if(!s_file || item_size <= WLAN_PCAP_REC_TS_LEN) return;
    uint32_t ts_us;
    memcpy(&ts_us, item, sizeof(ts_us));
    uint16_t len = (uint16_t)(item_size - WLAN_PCAP_REC_TS_LEN);
    wlan_pcap_write_packet(s_file, ts_us, item + WLAN_PCAP_REC_TS_LEN, len);
    // pcap-Record = Packet-Header (16) + Payload.
    s_bytes += (uint32_t)(sizeof(WlanPcapPacketHeader) + len);
    s_frames++;
}

static void wlan_pcap_rec_drain(void) {
    for(;;) {
        size_t item_size = 0;
        void* item = xRingbufferReceive(s_rb, &item_size, 0);
        if(!item) break;
        wlan_pcap_rec_write_item(item, item_size);
        vRingbufferReturnItem(s_rb, item);
    }
}

static int32_t wlan_pcap_rec_writer_task(void* ctx) {
    UNUSED(ctx);
    while(s_run) {
        size_t item_size = 0;
        void* item = xRingbufferReceive(s_rb, &item_size, pdMS_TO_TICKS(100));
        if(item) {
            wlan_pcap_rec_write_item(item, item_size);
            vRingbufferReturnItem(s_rb, item);
        }
    }
    // Restliche Frames nach dem Stop noch rausschreiben.
    wlan_pcap_rec_drain();
    return 0;
}

bool wlan_pcap_rec_start(const char* path) {
    if(s_active || !path) return false;

    s_rb_storage = heap_caps_malloc(WLAN_PCAP_REC_RB_SIZE, MALLOC_CAP_SPIRAM);
    if(!s_rb_storage) {
        FURI_LOG_E(TAG, "ring alloc failed (%u B PSRAM)", (unsigned)WLAN_PCAP_REC_RB_SIZE);
        return false;
    }
    s_rb = xRingbufferCreateStatic(
        WLAN_PCAP_REC_RB_SIZE, RINGBUF_TYPE_NOSPLIT, s_rb_storage, &s_rb_struct);
    if(!s_rb) {
        FURI_LOG_E(TAG, "ring create failed");
        heap_caps_free(s_rb_storage);
        s_rb_storage = NULL;
        return false;
    }

    s_storage = furi_record_open(RECORD_STORAGE);
    wlan_pcap_rec_mkdirs(s_storage, path);
    s_file = wlan_pcap_open(s_storage, path);
    if(!s_file) {
        FURI_LOG_E(TAG, "open failed: %s", path);
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
        vRingbufferDelete(s_rb);
        s_rb = NULL;
        heap_caps_free(s_rb_storage);
        s_rb_storage = NULL;
        return false;
    }

    s_frames = 0;
    s_bytes = sizeof(WlanPcapGlobalHeader);
    s_drops = 0;
    s_base_us = esp_timer_get_time();
    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;

    s_run = true;
    s_active = true;

    s_writer = furi_thread_alloc_ex("WlanPcapRec", 4096, wlan_pcap_rec_writer_task, NULL);
    furi_thread_set_priority(s_writer, FuriThreadPriorityNormal);
    furi_thread_start(s_writer);

    FURI_LOG_I(TAG, "start -> %s", s_path);
    return true;
}

void wlan_pcap_rec_stop(void) {
    if(!s_active) return;

    // Erst den Producer-Pfad sperren, dann den Writer beenden. Der Aufrufer hat
    // Promiscuous bereits deaktiviert, es kommt also kein neuer Frame mehr.
    s_active = false;
    s_run = false;

    if(s_writer) {
        furi_thread_join(s_writer);
        furi_thread_free(s_writer);
        s_writer = NULL;
    }
    if(s_file) {
        wlan_pcap_close(s_file);
        s_file = NULL;
    }
    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }
    if(s_rb) {
        vRingbufferDelete(s_rb);
        s_rb = NULL;
    }
    if(s_rb_storage) {
        heap_caps_free(s_rb_storage);
        s_rb_storage = NULL;
    }

    FURI_LOG_I(
        TAG,
        "stop: %s frames=%lu bytes=%lu drops=%lu",
        s_path,
        (unsigned long)s_frames,
        (unsigned long)s_bytes,
        (unsigned long)s_drops);
}

void wlan_pcap_rec_frame(const uint8_t* payload, uint16_t len) {
    if(!s_active || !s_rb || !payload || len == 0) return;

    uint16_t cap = (len > WLAN_PCAP_REC_SNAPLEN) ? (uint16_t)WLAN_PCAP_REC_SNAPLEN : len;

    int64_t delta = esp_timer_get_time() - s_base_us;
    if(delta < 0) delta = 0;
    uint32_t ts_us = (uint32_t)delta;

    memcpy(s_stage, &ts_us, sizeof(ts_us));
    memcpy(s_stage + WLAN_PCAP_REC_TS_LEN, payload, cap);

    size_t total = WLAN_PCAP_REC_TS_LEN + cap;
    if(xRingbufferSend(s_rb, s_stage, total, 0) != pdTRUE) {
        s_drops++;
    }
}

bool wlan_pcap_rec_is_active(void) {
    return s_active;
}

uint32_t wlan_pcap_rec_frames(void) {
    return s_frames;
}

uint32_t wlan_pcap_rec_bytes(void) {
    return s_bytes;
}

uint32_t wlan_pcap_rec_drops(void) {
    return s_drops;
}

const char* wlan_pcap_rec_path(void) {
    return s_path;
}

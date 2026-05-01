#include "ble_brute_log.h"

#include <furi.h>
#include <storage/storage.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#define TAG "BleBruteLog"

static const char CSV_HEADER[] =
    "timestamp,address,name,handle,status,status_name,value_hex\n";

struct BleBruteLog {
    Storage* storage;
    File* file;
};

const char* ble_brute_status_name(uint8_t status) {
    switch(status) {
    case 0x00: return "OK";
    case 0x01: return "InvalidHandle";
    case 0x02: return "NotPermit";
    case 0x03: return "WriteNotPermit";
    case 0x05: return "AuthReq";
    case 0x06: return "ReqNotSupp";
    case 0x07: return "InvalidOffset";
    case 0x08: return "AuthorReq";
    case 0x0C: return "InsufKeySize";
    case 0x0D: return "InvalidAttrLen";
    case 0x0F: return "EncReq";
    default:   return "Other";
    }
}

static void log_writef(BleBruteLog* log, const char* fmt, ...) {
    if(!log || !log->file) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    if(n > (int)sizeof(buf)) n = sizeof(buf);
    storage_file_write(log->file, buf, n);
}

static void write_csv_field_quoted(BleBruteLog* log, const char* s) {
    if(!log || !log->file) return;
    storage_file_write(log->file, "\"", 1);
    if(s) {
        const char* p = s;
        const char* run_start = s;
        while(*p) {
            if(*p == '"') {
                if(p > run_start) storage_file_write(log->file, run_start, p - run_start);
                storage_file_write(log->file, "\"\"", 2);
                run_start = p + 1;
            } else if(*p == '\n' || *p == '\r') {
                if(p > run_start) storage_file_write(log->file, run_start, p - run_start);
                storage_file_write(log->file, " ", 1);
                run_start = p + 1;
            }
            p++;
        }
        if(p > run_start) storage_file_write(log->file, run_start, p - run_start);
    }
    storage_file_write(log->file, "\"", 1);
}

static void write_addr(BleBruteLog* log, const esp_bd_addr_t addr) {
    log_writef(log, "%02X:%02X:%02X:%02X:%02X:%02X",
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void write_value_hex(BleBruteLog* log, const uint8_t* value, uint16_t len) {
    if(!value || len == 0) return;
    char buf[3];
    for(uint16_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02X", value[i]);
        storage_file_write(log->file, buf, 2);
    }
}

BleBruteLog* ble_brute_log_open(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BLE_BRUTE_DIR);

    bool existed = (storage_common_stat(storage, BLE_BRUTE_PATH, NULL) == FSE_OK);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, BLE_BRUTE_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        ESP_LOGE(TAG, "open append failed");
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    BleBruteLog* log = malloc(sizeof(BleBruteLog));
    log->storage = storage;
    log->file = file;

    if(!existed) {
        storage_file_write(file, CSV_HEADER, sizeof(CSV_HEADER) - 1);
    }
    return log;
}

void ble_brute_log_close(BleBruteLog* log) {
    if(!log) return;
    if(log->file) {
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) furi_record_close(RECORD_STORAGE);
    free(log);
}

static void write_device_prefix(BleBruteLog* log, const BleWalkDevice* device) {
    log_writef(log, "%lu,", (unsigned long)furi_get_tick());
    write_addr(log, device->addr);
    log_writef(log, ",");
    write_csv_field_quoted(log, device->name);
    log_writef(log, ",");
}

void ble_brute_log_session_marker(
    BleBruteLog* log,
    const BleWalkDevice* device,
    const char* event) {
    if(!log || !device || !event) return;
    write_device_prefix(log, device);
    // handle empty, status empty, status_name = event, value empty
    log_writef(log, ",,%s,\n", event);
}

void ble_brute_log_hit(
    BleBruteLog* log,
    const BleWalkDevice* device,
    uint16_t handle,
    uint8_t status,
    const uint8_t* value,
    uint16_t value_len) {
    if(!log || !device) return;
    write_device_prefix(log, device);
    log_writef(log, "0x%04X,0x%02X,%s,", handle, status, ble_brute_status_name(status));
    write_value_hex(log, value, value_len);
    storage_file_write(log->file, "\n", 1);
}

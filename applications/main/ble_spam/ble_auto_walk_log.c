#include "ble_auto_walk_log.h"
#include "ble_uuid_db.h"

#include <furi.h>
#include <storage/storage.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#define TAG "BleAutoWalkLog"

static const char CSV_HEADER[] =
    "timestamp,address,addr_type,rssi,name,status,"
    "service_uuid,service_name,char_uuid,char_name,properties,value_hex\n";

struct BleAutoWalkLog {
    Storage* storage;
    File* file;
};

// ---------------------------------------------------------------------------
// Seen-Set
// ---------------------------------------------------------------------------

void ble_auto_walk_seen_reset(BleAutoWalkSeenSet* set) {
    if(!set) return;
    memset(set, 0, sizeof(*set));
}

bool ble_auto_walk_seen_contains(const BleAutoWalkSeenSet* set, const esp_bd_addr_t addr) {
    if(!set) return false;
    for(uint16_t i = 0; i < set->count; i++) {
        if(memcmp(set->addrs[i], addr, 6) == 0) return true;
    }
    return false;
}

bool ble_auto_walk_seen_add(BleAutoWalkSeenSet* set, const esp_bd_addr_t addr) {
    if(!set) return false;
    if(ble_auto_walk_seen_contains(set, addr)) return true;
    if(set->count >= BLE_AUTO_WALK_SEEN_MAX) return false;
    memcpy(set->addrs[set->count], addr, 6);
    set->count++;
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Parses an "AA:BB:CC:DD:EE:FF" prefix from `s` into `out`. Returns true on success.
static bool parse_addr_prefix(const char* s, esp_bd_addr_t out) {
    if(!s) return false;
    for(int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]);
        if(hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        if(i < 5) {
            if(s[2] != ':') return false;
            s += 3;
        }
    }
    return true;
}

// Writes printf-style to file. Silent on failure.
static void log_writef(BleAutoWalkLog* log, const char* fmt, ...) {
    if(!log || !log->file) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    if(n > (int)sizeof(buf)) n = sizeof(buf);
    storage_file_write(log->file, buf, n);
}

static void write_csv_field_quoted(BleAutoWalkLog* log, const char* s) {
    if(!log || !log->file) return;
    storage_file_write(log->file, "\"", 1);
    if(s) {
        const char* p = s;
        const char* run_start = s;
        while(*p) {
            if(*p == '"') {
                if(p > run_start) {
                    storage_file_write(log->file, run_start, p - run_start);
                }
                storage_file_write(log->file, "\"\"", 2);
                run_start = p + 1;
            } else if(*p == '\n' || *p == '\r') {
                if(p > run_start) {
                    storage_file_write(log->file, run_start, p - run_start);
                }
                storage_file_write(log->file, " ", 1);
                run_start = p + 1;
            }
            p++;
        }
        if(p > run_start) {
            storage_file_write(log->file, run_start, p - run_start);
        }
    }
    storage_file_write(log->file, "\"", 1);
}

static void format_uuid(const esp_bt_uuid_t* uuid, char* buf, size_t buf_len) {
    if(uuid->len == ESP_UUID_LEN_16) {
        snprintf(buf, buf_len, "%04X", uuid->uuid.uuid16);
    } else if(uuid->len == ESP_UUID_LEN_32) {
        snprintf(buf, buf_len, "%08lX", (unsigned long)uuid->uuid.uuid32);
    } else if(uuid->len == ESP_UUID_LEN_128) {
        const uint8_t* u = uuid->uuid.uuid128;
        snprintf(
            buf, buf_len,
            "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
            u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
    } else {
        snprintf(buf, buf_len, "?");
    }
}

static void write_addr(BleAutoWalkLog* log, const esp_bd_addr_t addr) {
    log_writef(log, "%02X:%02X:%02X:%02X:%02X:%02X",
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void write_value_hex(BleAutoWalkLog* log, const uint8_t* value, uint16_t len) {
    if(!value || len == 0) return;
    char buf[3];
    for(uint16_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02X", value[i]);
        storage_file_write(log->file, buf, 2);
    }
}

// ---------------------------------------------------------------------------
// Seen-Set bootstrap (read existing CSV)
// ---------------------------------------------------------------------------

static void load_existing_seen(Storage* storage, BleAutoWalkSeenSet* out_seen) {
    if(storage_common_stat(storage, BLE_AUTO_WALK_PATH, NULL) != FSE_OK) return;

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, BLE_AUTO_WALK_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return;
    }

    // Read line-by-line. We only need the address (column 2), so a small ring is fine.
    char chunk[256];
    char line[128];
    uint16_t line_len = 0;
    bool first_line = true;

    while(true) {
        uint16_t got = storage_file_read(file, chunk, sizeof(chunk));
        if(got == 0) break;

        for(uint16_t i = 0; i < got; i++) {
            char c = chunk[i];
            if(c == '\n' || c == '\r') {
                if(line_len > 0) {
                    line[line_len] = '\0';
                    if(first_line) {
                        first_line = false; // skip header
                    } else {
                        // line layout: "<ts>,<AA:BB:..>,..."
                        const char* comma = strchr(line, ',');
                        if(comma) {
                            esp_bd_addr_t addr;
                            if(parse_addr_prefix(comma + 1, addr)) {
                                ble_auto_walk_seen_add(out_seen, addr);
                            }
                        }
                    }
                    line_len = 0;
                }
            } else if(line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    if(line_len > 0 && !first_line) {
        line[line_len] = '\0';
        const char* comma = strchr(line, ',');
        if(comma) {
            esp_bd_addr_t addr;
            if(parse_addr_prefix(comma + 1, addr)) {
                ble_auto_walk_seen_add(out_seen, addr);
            }
        }
    }

    storage_file_close(file);
    storage_file_free(file);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

BleAutoWalkLog* ble_auto_walk_log_open(BleAutoWalkSeenSet* out_seen) {
    if(out_seen) ble_auto_walk_seen_reset(out_seen);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, BLE_AUTO_WALK_DIR);

    bool existed = (storage_common_stat(storage, BLE_AUTO_WALK_PATH, NULL) == FSE_OK);
    if(existed && out_seen) {
        load_existing_seen(storage, out_seen);
    }

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, BLE_AUTO_WALK_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        ESP_LOGE(TAG, "open append failed");
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    BleAutoWalkLog* log = malloc(sizeof(BleAutoWalkLog));
    log->storage = storage;
    log->file = file;

    if(!existed) {
        storage_file_write(file, CSV_HEADER, sizeof(CSV_HEADER) - 1);
    }

    ESP_LOGI(TAG, "log opened, seen=%u", out_seen ? out_seen->count : 0);
    return log;
}

void ble_auto_walk_log_close(BleAutoWalkLog* log) {
    if(!log) return;
    if(log->file) {
        storage_file_close(log->file);
        storage_file_free(log->file);
    }
    if(log->storage) furi_record_close(RECORD_STORAGE);
    free(log);
}

static void write_device_prefix(
    BleAutoWalkLog* log,
    const BleWalkDevice* device,
    const char* status) {
    log_writef(log, "%lu,", (unsigned long)furi_get_tick());
    write_addr(log, device->addr);
    log_writef(log, ",%u,%d,", device->addr_type, device->rssi);
    write_csv_field_quoted(log, device->name);
    log_writef(log, ",%s,", status);
}

void ble_auto_walk_log_device_marker(
    BleAutoWalkLog* log,
    const BleWalkDevice* device,
    const char* status) {
    if(!log || !device || !status) return;
    write_device_prefix(log, device, status);
    // empty service/char/properties/value columns
    storage_file_write(log->file, ",,,,,\n", 6);
}

void ble_auto_walk_log_char_value(
    BleAutoWalkLog* log,
    const BleWalkDevice* device,
    const BleWalkService* service,
    const BleWalkChar* chr,
    const uint8_t* value,
    uint16_t value_len) {
    if(!log || !device || !service || !chr) return;

    write_device_prefix(log, device, "ok");

    char service_uuid[40];
    char char_uuid[40];
    format_uuid(&service->uuid, service_uuid, sizeof(service_uuid));
    format_uuid(&chr->uuid, char_uuid, sizeof(char_uuid));

    const char* svc_name = ble_uuid_db_lookup_service(&service->uuid);
    const char* chr_name = ble_uuid_db_lookup_char(&chr->uuid);

    log_writef(log, "%s,", service_uuid);
    write_csv_field_quoted(log, svc_name ? svc_name : "");
    log_writef(log, ",%s,", char_uuid);
    write_csv_field_quoted(log, chr_name ? chr_name : "");
    log_writef(log, ",0x%02X,", chr->properties);

    write_value_hex(log, value, value_len);
    storage_file_write(log->file, "\n", 1);
}

#include "ble_uuid_db.h"

#include <ctype.h>
#include <esp_log.h>
#include <furi.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>

#define TAG "BleUuidDb"

#define USER_MAP_MAX_FILE_SIZE (96 * 1024)
#define USER_MAP_MAX_NAME_LEN 40

// ---------------------------------------------------------------------------
// Built-in Bluetooth SIG fallback tables (used when no SD files present)
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t uuid;
    const char* name;
} sig_entry_t;

static const sig_entry_t SIG_SERVICES[] = {
    {0x1800, "Generic Access"},      {0x1801, "Generic Attribute"},
    {0x1802, "Immediate Alert"},     {0x1803, "Link Loss"},
    {0x1804, "Tx Power"},            {0x1805, "Current Time"},
    {0x1806, "Reference Time Upd"},  {0x1808, "Glucose"},
    {0x1809, "Health Thermometer"},  {0x180A, "Device Information"},
    {0x180D, "Heart Rate"},          {0x180E, "Phone Alert Status"},
    {0x180F, "Battery"},             {0x1810, "Blood Pressure"},
    {0x1811, "Alert Notification"},  {0x1812, "HID"},
    {0x1813, "Scan Parameters"},     {0x1814, "Running Speed"},
    {0x1816, "Cycling Speed"},       {0x1818, "Cycling Power"},
    {0x1819, "Location/Nav"},        {0x181A, "Environmental"},
    {0x181B, "Body Composition"},    {0x181C, "User Data"},
    {0x181D, "Weight Scale"},        {0x181E, "Bond Management"},
    {0x181F, "Cont. Glucose"},       {0x1820, "IP Support"},
    {0x1822, "Pulse Oximeter"},      {0x1826, "Fitness Machine"},
    {0x183A, "Insulin Delivery"},    {0xFE2C, "Google Fast Pair"},
    {0xFD6F, "Exposure Notif."},     {0xFEED, "Tile"},
    {0xFD84, "Tile"},                {0xFDF0, "Samsung SmartThings"},
    {0xFDAB, "Nordic UART"},
};

static const sig_entry_t SIG_CHARS[] = {
    {0x2A00, "Device Name"},     {0x2A01, "Appearance"},
    {0x2A04, "PPCP"},            {0x2A19, "Battery Level"},
    {0x2A23, "System ID"},       {0x2A24, "Model Number"},
    {0x2A25, "Serial Number"},   {0x2A26, "Firmware Rev"},
    {0x2A27, "Hardware Rev"},    {0x2A28, "Software Rev"},
    {0x2A29, "Manufacturer"},    {0x2A2A, "IEEE 11073 Cert"},
    {0x2A2B, "Current Time"},    {0x2A37, "HR Measurement"},
    {0x2A38, "Body Sensor Loc"}, {0x2A39, "HR Control Pt"},
    {0x2A4A, "HID Info"},        {0x2A4B, "Report Map"},
    {0x2A4C, "HID Control Pt"},  {0x2A4D, "Report"},
    {0x2A4E, "Protocol Mode"},   {0x2A50, "PnP ID"},
    {0x2A5B, "CSC Measurement"}, {0x2A63, "CP Measurement"},
    {0x2A6D, "Pressure"},        {0x2A6E, "Temperature"},
    {0x2A6F, "Humidity"},
};

static const char* sig_lookup(const sig_entry_t* table, size_t n, uint16_t u) {
    for(size_t i = 0; i < n; ++i) {
        if(table[i].uuid == u) return table[i].name;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Dynamic UUID maps (grown via realloc as files are parsed)
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t len;     // 2 or 16
    uint8_t bytes[16]; // LE order, matches esp_bt_uuid_t.uuid.uuid128
    char name[USER_MAP_MAX_NAME_LEN];
} user_entry_t;

typedef struct {
    user_entry_t* entries;
    uint16_t count;
    uint16_t capacity;
} uuid_map_t;

static uuid_map_t s_services = {0};
static uuid_map_t s_chars = {0};
static uuid_map_t s_descriptors = {0};
static uuid_map_t s_members = {0};
static bool s_initialized = false;

static void map_append(uuid_map_t* map, const user_entry_t* e) {
    if(map->count >= map->capacity) {
        uint16_t newcap = (map->capacity == 0) ? 32 : (uint16_t)(map->capacity * 2);
        user_entry_t* newp =
            realloc(map->entries, (size_t)newcap * sizeof(user_entry_t));
        if(!newp) return; // OOM: drop this entry silently
        map->entries = newp;
        map->capacity = newcap;
    }
    map->entries[map->count++] = *e;
}

static void map_free(uuid_map_t* map) {
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->capacity = 0;
}

// ---------------------------------------------------------------------------
// Parsing helpers
// ---------------------------------------------------------------------------

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t strip_dashes(char* s) {
    size_t r = 0, w = 0;
    while(s[r]) {
        if(s[r] != '-') s[w++] = s[r];
        r++;
    }
    s[w] = '\0';
    return w;
}

// Parse "180F" / "0x180F" / "0000180F-0000-1000-8000-00805F9B34FB" → bytes (LE).
// Returns 2, 16, or 0 on error.
static uint8_t parse_uuid_token(const char* tok, uint8_t out_bytes[16]) {
    char buf[40];
    size_t tl = strlen(tok);
    if(tl >= sizeof(buf)) return 0;
    memcpy(buf, tok, tl + 1);

    char* p = buf;
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    size_t n = strip_dashes(p);

    if(n == 4) {
        int hi1 = hex_nibble(p[0]);
        int hi2 = hex_nibble(p[1]);
        int lo1 = hex_nibble(p[2]);
        int lo2 = hex_nibble(p[3]);
        if(hi1 < 0 || hi2 < 0 || lo1 < 0 || lo2 < 0) return 0;
        uint16_t u = (hi1 << 12) | (hi2 << 8) | (lo1 << 4) | lo2;
        memset(out_bytes, 0, 16);
        out_bytes[0] = u & 0xFF;
        out_bytes[1] = (u >> 8) & 0xFF;
        return 2;
    }
    if(n == 32) {
        for(size_t i = 0; i < 16; ++i) {
            int hi = hex_nibble(p[i * 2]);
            int lo = hex_nibble(p[i * 2 + 1]);
            if(hi < 0 || lo < 0) return 0;
            out_bytes[15 - i] = (uint8_t)((hi << 4) | lo);
        }
        return 16;
    }
    return 0;
}

static char* trim(char* s) {
    while(*s && isspace((unsigned char)*s)) s++;
    if(!*s) return s;
    char* end = s + strlen(s) - 1;
    while(end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

// Strip an optional trailing " [source-tag]" from the name.
// e.g. "Battery  [service:gss]" → "Battery"
static void strip_source_tag(char* s) {
    char* lb = strrchr(s, '[');
    if(!lb) return;
    char* rb = strchr(lb, ']');
    if(!rb) return;
    char* p = lb;
    while(p > s && (p[-1] == ' ' || p[-1] == '\t')) p--;
    *p = '\0';
}

static void parse_line(char* line, uuid_map_t* map) {
    char* trimmed = trim(line);
    if(!*trimmed || *trimmed == '#') return;

    char* sep = trimmed;
    while(*sep && !isspace((unsigned char)*sep)) sep++;
    if(!*sep) return;
    *sep = '\0';
    char* name = trim(sep + 1);
    if(!*name) return;

    strip_source_tag(name);
    name = trim(name);
    if(!*name) return;

    user_entry_t e = {0};
    uint8_t kind = parse_uuid_token(trimmed, e.bytes);
    if(kind == 0) return;

    e.len = kind;
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    map_append(map, &e);
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

static bool load_file(const char* path, uuid_map_t* map) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool opened = false;
    bool ok = false;
    char* buf = NULL;

    opened = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(!opened) {
        ESP_LOGI(TAG, "no map at %s", path);
        goto cleanup;
    }

    uint64_t fsize = storage_file_size(file);
    if(fsize == 0 || fsize > USER_MAP_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "%s size %llu out of range", path, fsize);
        goto cleanup;
    }

    buf = malloc((size_t)fsize + 1);
    if(!buf) goto cleanup;

    // Read in chunks — storage_file_read takes uint16_t len
    size_t total = 0;
    while(total < fsize) {
        size_t want = fsize - total;
        if(want > 0xF000) want = 0xF000;
        uint16_t got = storage_file_read(file, buf + total, (uint16_t)want);
        if(got == 0) break;
        total += got;
    }
    buf[total] = '\0';

    char* save = NULL;
    char* line = strtok_r(buf, "\r\n", &save);
    while(line) {
        parse_line(line, map);
        line = strtok_r(NULL, "\r\n", &save);
    }
    ok = true;

cleanup:
    free(buf);
    if(opened) storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ble_uuid_db_init(void) {
    if(s_initialized) return;
    s_initialized = true;

    load_file(BLE_UUID_DB_DIR "/services.txt", &s_services);
    load_file(BLE_UUID_DB_DIR "/chars.txt", &s_chars);
    load_file(BLE_UUID_DB_DIR "/descriptors.txt", &s_descriptors);
    load_file(BLE_UUID_DB_DIR "/members.txt", &s_members);

    ESP_LOGI(
        TAG,
        "loaded uuid db: services=%u chars=%u desc=%u members=%u",
        s_services.count,
        s_chars.count,
        s_descriptors.count,
        s_members.count);
}

void ble_uuid_db_deinit(void) {
    map_free(&s_services);
    map_free(&s_chars);
    map_free(&s_descriptors);
    map_free(&s_members);
    s_initialized = false;
}

static const char* map_lookup(const uuid_map_t* map, const esp_bt_uuid_t* uuid) {
    if(!map->entries || map->count == 0) return NULL;

    if(uuid->len == ESP_UUID_LEN_16) {
        uint8_t lo = uuid->uuid.uuid16 & 0xFF;
        uint8_t hi = (uuid->uuid.uuid16 >> 8) & 0xFF;
        for(uint16_t i = 0; i < map->count; ++i) {
            const user_entry_t* e = &map->entries[i];
            if(e->len == 2 && e->bytes[0] == lo && e->bytes[1] == hi) return e->name;
        }
    } else if(uuid->len == ESP_UUID_LEN_128) {
        for(uint16_t i = 0; i < map->count; ++i) {
            const user_entry_t* e = &map->entries[i];
            if(e->len == 16 && memcmp(e->bytes, uuid->uuid.uuid128, 16) == 0) return e->name;
        }
    }
    return NULL;
}

const char* ble_uuid_db_lookup_service(const esp_bt_uuid_t* uuid) {
    const char* hit = map_lookup(&s_services, uuid);
    if(hit) return hit;
    hit = map_lookup(&s_members, uuid);
    if(hit) return hit;

    if(uuid->len == ESP_UUID_LEN_16) {
        return sig_lookup(
            SIG_SERVICES, sizeof(SIG_SERVICES) / sizeof(SIG_SERVICES[0]), uuid->uuid.uuid16);
    }
    return NULL;
}

const char* ble_uuid_db_lookup_char(const esp_bt_uuid_t* uuid) {
    const char* hit = map_lookup(&s_chars, uuid);
    if(hit) return hit;

    if(uuid->len == ESP_UUID_LEN_16) {
        return sig_lookup(SIG_CHARS, sizeof(SIG_CHARS) / sizeof(SIG_CHARS[0]), uuid->uuid.uuid16);
    }
    return NULL;
}

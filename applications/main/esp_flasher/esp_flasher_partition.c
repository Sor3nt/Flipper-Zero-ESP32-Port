#include "esp_flasher_partition.h"

#include <stdarg.h>

// Partition binary entry format (ESP-IDF)
#define PART_ENTRY_SIZE      32
#define PART_MAX_ENTRIES     (0xC00 / PART_ENTRY_SIZE)
#define PART_MAGIC_BYTE0     0xAA
#define PART_MAGIC_BYTE1     0x50
#define PART_MD5_MAGIC0      0xEB
#define PART_TYPE_APP        0x00
#define PART_TYPE_DATA       0x01
#define PART_SUBTYPE_NVS     0x02
#define PART_SUBTYPE_OTADATA 0x00
#define PART_SUBTYPE_FACTORY 0x00
#define PART_SUBTYPE_OTA_0   0x10
#define PART_SUBTYPE_OTA_1   0x11

static size_t esp_flasher_partition_append(char* buf, size_t size, size_t pos, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

static size_t
    esp_flasher_partition_append(char* buf, size_t size, size_t pos, const char* format, ...) {
    if(!buf || pos >= size) return pos;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buf + pos, size - pos, format, args);
    va_end(args);
    if(written < 0) return pos;
    return MIN(pos + (size_t)written, size);
}

bool esp_flasher_partition_parse(
    Storage* storage,
    const char* path,
    EspFlasherPartitionInfo* info,
    char* text,
    size_t text_size) {
    memset(info, 0, sizeof(*info));
    if(text && text_size) text[0] = '\0';

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        return false;
    }
    const uint64_t file_size = storage_file_size(f);
    if(file_size == 0 || file_size > 0xC00 || file_size % PART_ENTRY_SIZE != 0) {
        storage_file_close(f);
        storage_file_free(f);
        return false;
    }

    uint8_t entry[PART_ENTRY_SIZE];
    size_t pos = esp_flasher_partition_append(text, text_size, 0, "Partition file:\n");
    bool valid = true;

    for(int n = 0; n < PART_MAX_ENTRIES; n++) {
        if(storage_file_read(f, entry, PART_ENTRY_SIZE) != PART_ENTRY_SIZE) break;
        if(entry[0] == 0xFF) break; // end of table padding
        if(entry[0] == PART_MD5_MAGIC0 && entry[1] == PART_MD5_MAGIC0) continue; // MD5 entry
        if(entry[0] != PART_MAGIC_BYTE0 || entry[1] != PART_MAGIC_BYTE1) {
            valid = false;
            break;
        }

        const uint8_t type = entry[2];
        const uint8_t subtype = entry[3];
        const uint32_t offset = (uint32_t)entry[4] | ((uint32_t)entry[5] << 8) |
                                ((uint32_t)entry[6] << 16) | ((uint32_t)entry[7] << 24);
        const uint32_t size = (uint32_t)entry[8] | ((uint32_t)entry[9] << 8) |
                              ((uint32_t)entry[10] << 16) | ((uint32_t)entry[11] << 24);
        const uint64_t end = (uint64_t)offset + size;
        if(end > UINT32_MAX) {
            valid = false;
            break;
        }
        if(end > info->end) info->end = (uint32_t)end;

        char name[17];
        memcpy(name, &entry[12], 16);
        name[16] = '\0';

        // map type/subtype to a known flash slot; the first app partition is firmware A
        int slot = -1;
        const char* slot_name = NULL;
        if(type == PART_TYPE_APP &&
           (subtype == PART_SUBTYPE_FACTORY || subtype == PART_SUBTYPE_OTA_0) &&
           !info->slot_addrs[SelectedFlashAppA]) {
            slot = SelectedFlashAppA;
            slot_name = "FirmwareA";
        } else if(
            type == PART_TYPE_APP && subtype == PART_SUBTYPE_OTA_1 &&
            !info->slot_addrs[SelectedFlashAppB]) {
            slot = SelectedFlashAppB;
            slot_name = "FirmwareB";
        } else if(
            type == PART_TYPE_DATA && subtype == PART_SUBTYPE_OTADATA &&
            !info->slot_addrs[SelectedFlashBootApp0]) {
            slot = SelectedFlashBootApp0;
            slot_name = "boot_app0";
        } else if(
            type == PART_TYPE_DATA && subtype == PART_SUBTYPE_NVS &&
            !info->slot_addrs[SelectedFlashNvs]) {
            slot = SelectedFlashNvs;
            slot_name = "NVS";
        }

        if(slot != -1) {
            info->slot_addrs[slot] = offset;
            pos = esp_flasher_partition_append(
                text, text_size, pos, "%s: 0x%lx [%s]\n", name, offset, slot_name);
            info->matched++;
        } else {
            pos = esp_flasher_partition_append(
                text, text_size, pos, "%s: 0x%lx [-]\n", name, offset);
            info->other++;
        }
    }

    storage_file_close(f);
    storage_file_free(f);

    esp_flasher_partition_append(
        text,
        text_size,
        pos,
        "Needs %lu KB flash\n\n%d matched, %d other\nApply these addresses?",
        info->end / 1024,
        info->matched,
        info->other);

    return valid && info->matched + info->other > 0;
}

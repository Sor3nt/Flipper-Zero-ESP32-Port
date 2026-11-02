#pragma once

#include "esp_flasher_app_i.h"

typedef struct {
    /* Flash offset per slot taken from the table, 0 = not in the table */
    uint32_t slot_addrs[NUM_FLASH_OPTIONS];
    /* First byte past the highest partition: the minimum flash size the layout needs */
    uint32_t end;
    int matched;
    int other;
} EspFlasherPartitionInfo;

/* Parses an ESP-IDF partition table binary. text (optional) receives a human-readable
 * summary. Returns false when the file cannot be read or holds no partition entry. */
bool esp_flasher_partition_parse(
    Storage* storage,
    const char* path,
    EspFlasherPartitionInfo* info,
    char* text,
    size_t text_size);


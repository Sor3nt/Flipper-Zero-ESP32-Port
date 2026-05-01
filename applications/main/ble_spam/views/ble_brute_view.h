#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char target_name[32];
    char target_addr[18];
    uint16_t current_handle;
    uint16_t total_handles;
    uint16_t handles_done;
    uint16_t hit_count;
    uint16_t last_hit_handle;
    uint8_t last_hit_status;
    uint16_t last_hit_value_len;
    bool running;
    bool failed;
    bool done;
} BleBruteModel;

View* ble_brute_view_alloc(void);
void ble_brute_view_free(View* view);

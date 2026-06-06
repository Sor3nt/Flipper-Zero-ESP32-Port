#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_protocol.h"

/* Persistent state in NVS: the single master we are paired with. */

typedef struct {
    uint8_t mac[BUDDY_MAC_LEN];
    char name[BUDDY_NAME_MAX + 1];
    bool valid;
} BuddyMaster;

/* Initializes NVS (also required before WiFi). Safe to call once at boot. */
void buddy_store_init(void);

bool buddy_store_load_master(BuddyMaster* out);
bool buddy_store_save_master(const uint8_t mac[BUDDY_MAC_LEN], const char* name);
void buddy_store_clear_master(void);

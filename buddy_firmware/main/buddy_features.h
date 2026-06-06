#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "buddy_protocol.h"

/* A capability the buddy advertises and the master can start/stop remotely.
 *
 * `id` doubles as the bit position in the capabilities bitmask (so 0..31).
 * start()/stop() are invoked from the buddy worker task when the master sends
 * BuddyWireFeatureStart / BuddyWireFeatureStop. A feature owns its status
 * reporting: it should call buddy_node_feature_status() to tell the master what
 * happened (running / stopped / error / data). `args` is the raw start payload. */
typedef struct {
    uint8_t id;
    const char* name;
    esp_err_t (*start)(const uint8_t* args, uint8_t arg_len);
    esp_err_t (*stop)(void);
    /* Optional: läuft dieses Feature gerade? NULL = nie "laufend" (z.B. Echo,
     * das nur kurz antwortet). Wird für die running-Mask benutzt, damit der
     * Master den aktuellen Status live abfragen kann (kein Caching am Master). */
    bool (*is_running)(void);
} BuddyFeature;

/* Bitmask of all supported feature ids (advertised in Discover/Pair responses). */
uint32_t buddy_features_caps(void);

/* Bitmask der gerade laufenden Features (Bit i = Feature-ID i läuft). Der Buddy
 * hängt das an seine FeatureList-Antwort an — der Master ist damit nie auf einen
 * gecachten Status angewiesen. */
uint32_t buddy_features_running_mask(void);

/* Look up a feature by id, or NULL. */
const BuddyFeature* buddy_features_get(uint8_t id);

/* The full registry (array of pointers). */
const BuddyFeature* const* buddy_features_list(size_t* out_count);

/* Stoppt alle gerade laufenden Features (z.B. bei Disconnect/Unpair). */
void buddy_features_stop_all(void);

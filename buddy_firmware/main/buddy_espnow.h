#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_protocol.h"

/* Thin ESP-NOW transport: brings up WiFi (STA, fixed channel) + ESP-NOW, and
 * pumps received frames to `cb` from a dedicated worker task (NOT the WiFi
 * callback context, so the handler may do blocking work and send replies). */

/* Called for every received buddy frame. Runs in the buddy worker task. */
typedef void (*buddy_frame_cb_t)(
    const uint8_t src_mac[BUDDY_MAC_LEN],
    const uint8_t* data,
    int len,
    void* ctx);

/* Init WiFi + ESP-NOW and start the worker. Returns false on hard failure. */
bool buddy_espnow_init(buddy_frame_cb_t cb, void* ctx);

/* Send a unicast frame (auto-registers the peer on first use). */
bool buddy_espnow_send(const uint8_t mac[BUDDY_MAC_LEN], const uint8_t* data, uint8_t len);

/* Make sure `mac` is a registered ESP-NOW peer (idempotent). */
void buddy_espnow_ensure_peer(const uint8_t mac[BUDDY_MAC_LEN]);

/* Our own STA MAC (valid after buddy_espnow_init). */
void buddy_espnow_get_self_mac(uint8_t out[BUDDY_MAC_LEN]);

/* Liveness über den ESP-NOW-Send-ACK: ms seit dem letzten erfolgreich
 * bestätigten Unicast-Send. Eine streamende Feature (Capture) nutzt das als
 * Watchdog — bleibt der Wert lange hoch, ist der Master weg/off-channel. */
uint32_t buddy_espnow_ms_since_send_ok(void);

/* Watchdog-Baseline auf "jetzt" setzen (z.B. beim Start eines Captures). */
void buddy_espnow_note_alive(void);

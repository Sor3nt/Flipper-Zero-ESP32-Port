#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_protocol.h"

/* Reliable result store: results (e.g. a captured WPA handshake) are queued here
 * and retained until the master confirms each one with BuddyWireResultAck. They
 * survive master absence — pump() re-sends every pending result whenever the
 * master is reachable; ack() drops the matching one. */

#define BUDDY_RESULT_DATA_MAX 48
#define BUDDY_RESULT_MAX      8

void buddy_results_init(void);

/* Queue a new result (assigns a local id). `len` is clamped to the max. When the
 * queue is full the oldest pending result is evicted. */
bool buddy_results_push(uint8_t type, const uint8_t* data, uint8_t len);

/* Send every pending result to the master (BuddyWireResult). Call when reachable. */
void buddy_results_pump(const uint8_t master_mac[BUDDY_MAC_LEN]);

/* Master confirmed this id was stored → drop it. */
void buddy_results_ack(uint8_t id);

bool buddy_results_has_pending(void);

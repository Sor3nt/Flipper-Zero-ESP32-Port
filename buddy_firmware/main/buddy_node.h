#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buddy_protocol.h"

/* The buddy "brain": owns pairing state and dispatches master commands.
 * All entry points run in the buddy worker task (see buddy_espnow). */

/* Compute our device name and load any stored master. Call before espnow init. */
void buddy_node_init(void);

/* After espnow is up: pre-register the stored master peer and log readiness. */
void buddy_node_ready(void);

/* Frame handler — pass to buddy_espnow_init(). */
void buddy_node_on_frame(const uint8_t src[BUDDY_MAC_LEN], const uint8_t* data, int len, void* ctx);

/* Send a feature status frame to the currently paired master (no-op if none).
 * Features call this to report results back. */
void buddy_node_feature_status(uint8_t feat_id, uint8_t state, const uint8_t* data, uint8_t len);

/* Our advertised device name (e.g. "Buddy-A1B2C3"). */
const char* buddy_node_self_name(void);

/* Copy the currently paired master's MAC into out. Returns false if unpaired.
 * Used by streaming features (e.g. pcap capture) to send straight to the master. */
bool buddy_node_get_master_mac(uint8_t out[BUDDY_MAC_LEN]);

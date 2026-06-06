#pragma once

#include <stdint.h>

/* ───────────────────────────────────────────────────────────────────────────
 * Wire protocol — shared contract with the Flipper-Zero-ESP32 master.
 *
 * Source of truth on the master side:
 *   applications/services/desktop/helpers/mesh_service.c
 *
 * Frame layout (ESP-NOW payload, max 64 bytes):
 *   [0] magic  = 0x4D ('M')
 *   [1] type   = BuddyWireType
 *   [2..] type-specific payload
 *
 * Sub-encodings used in payloads:
 *   name : [len:1][bytes:len]          (len <= BUDDY_NAME_MAX)
 *   caps : [bits:4 little-endian]      (bit i set  =>  feature id i supported)
 *
 * Types 1..6 are IDENTICAL to the master's MeshWireType and MUST NOT change —
 * that is what makes plain discovery + pairing work without touching the master.
 * Types 7+ are buddy extensions for feature control; the current master simply
 * ignores them (and ignores the trailing `caps` block we append to 2 & 4,
 * because its name parser stops after the name). Teach the master to read them
 * when wiring up the feature UI.
 * ─────────────────────────────────────────────────────────────────────────── */

#define BUDDY_MAGIC       0x4D /* 'M' */
#define BUDDY_MAX_PAYLOAD 64
#define BUDDY_MAC_LEN     6
#define BUDDY_NAME_MAX    32 /* name length on the wire, without NUL */

typedef enum {
    /* ── core (must match master 1:1) ── */
    BuddyWireDiscoverReq   = 1, /* master → broadcast : [name]                  */
    BuddyWireDiscoverRsp   = 2, /* buddy  → master    : [name][caps]            */
    BuddyWirePairReq       = 3, /* master → buddy     : [name]                  */
    BuddyWirePairRsp       = 4, /* buddy  → master    : [accepted:1][name][caps]*/
    BuddyWireDisconnect    = 5, /* master → buddy     : (none)                  */
    BuddyWireDisconnectAck = 6, /* buddy  → master    : (none)                  */

    /* ── extensions: feature control (master learns these later) ── */
    BuddyWireFeatureQuery  = 7,  /* master → buddy  : (none) request full list   */
    BuddyWireFeatureList   = 8,  /* buddy  → master : [count:1]{[id:1][name]}[running:4] */
    BuddyWireFeatureStart  = 9,  /* master → buddy  : [id:1][arg_len:1][args]    */
    BuddyWireFeatureStop   = 10, /* master → buddy  : [id:1]                     */
    BuddyWireFeatureStatus = 11, /* buddy  → master : [id:1][state:1][len:1][data]*/

    /* Streamed capture: a (possibly fragmented) raw 802.11 frame, buddy→master.
     * The master reassembles frags 0..cnt-1 of one `seq` and appends the frame
     * to a .pcap on SD. Header = magic,type,seq,frag_idx,frag_cnt (5 bytes) →
     * up to BUDDY_MAX_PAYLOAD-5 data bytes per fragment. Timestamp is stamped by
     * the master on receipt (exact capture time isn't needed for cracking). */
    BuddyWirePcapFrame     = 12, /* buddy  → master : [seq:1][frag_idx:1][frag_cnt:1][data] */

    /* Reliable result delivery: a stored handshake the buddy keeps re-sending
     * until the master confirms it with BuddyWireResultAck (then the buddy drops
     * it + clears NVS). Survives master absence/buddy reboot. `id` is a buddy-local
     * handle for ACK matching. Fragmented (the payload is bigger than one frame):
     * the master reassembles frags 0..cnt-1 of one `id` into:
     *   [type:1][ssid_len:1][ssid][frame_count:1]{ [msg_num:1][len:2 LE][bytes] }
     * (msg_num 0 = beacon, 1..4 = EAPOL M1..M4) and writes a .pcap from it. */
    BuddyWireResult        = 13, /* buddy  → master : [id:1][frag_idx:1][frag_cnt:1][chunk] */
    BuddyWireResultAck     = 14, /* master → buddy  : [id:1]                      */
} BuddyWireType;

/* Bytes of a PcapFrame fragment consumed by the header (after magic+type). */
#define BUDDY_PCAP_HDR 5

/* State byte carried in BuddyWireFeatureStatus. */
typedef enum {
    BuddyFeatStateStopped = 0,
    BuddyFeatStateRunning = 1,
    BuddyFeatStateError   = 2,
    BuddyFeatStateData    = 3, /* status carries feature output in [data] */
} BuddyFeatState;

/* Result type carried in BuddyWireResult. */
typedef enum {
    BuddyResultHandshake = 1, /* WPA handshake captured; [data] = SSID string */
} BuddyResultType;

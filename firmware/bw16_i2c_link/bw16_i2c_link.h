/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BW16_I2C_LINK_H
#define BW16_I2C_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BWI_ADDRESS 0x42U
#define BWI_FRAME_SIZE 64U
#define BWI_PAYLOAD_SIZE 40U
#define BWI_TX_CAPACITY 32768U
#define BWI_COMMAND_CAPACITY 8U
#define BWI_OP_OPEN 1U
#define BWI_OP_POLL 2U
#define BWI_OP_COMMAND 3U
#define BWI_OP_STOP 4U
#define BWI_FLAG_READY 1U
#define BWI_FLAG_FAULT 2U

typedef struct {
    uint8_t op; /* Request opcode, or READY/FAULT bits in a response. */
    uint32_t session, sequence, ack;
    uint8_t length;
    uint8_t payload[BWI_PAYLOAD_SIZE];
} BwiFrame;
typedef struct {
    uint8_t length; /* Includes final LF, excludes the convenience NUL. */
    char data[BWI_PAYLOAD_SIZE + 1U];
} BwiCommand;
typedef struct {
    uint32_t session, command_ack, response_sequence, response_acked;
    uint32_t tx_used;
    uint16_t tx_head, tx_tail;
    uint8_t response_length, command_head, command_tail, command_count;
    bool open_pending, stop_pending, ready, fault;
    BwiCommand commands[BWI_COMMAND_CAPACITY];
    uint8_t tx[BWI_TX_CAPACITY];
} BwiServer;

/* Exactly 64 bytes: BI, v1, opcode/flags, LE session/seq/ack, length,
 * three zero bytes, 40-byte zero-padded payload, LE CRC32/IEEE of bytes 0..59.
 * Decode rejects bad length/magic/version/reserved/padding/CRC and leaves
 * output untouched on failure. Codec permits flags=0 and session=0 replies. */
uint32_t bwi_crc32(const uint8_t* data, size_t length);
bool bwi_encode(uint8_t out[BWI_FRAME_SIZE], const BwiFrame* frame);
bool bwi_decode(const uint8_t* raw, size_t length, BwiFrame* frame);

/* All server calls must be serialized by the adapter (IRQ versus main loop).
 * No allocation, hardware access, blocking, or native-command execution occurs.
 * NEW nonzero OPEN(seq=ack=len=0) flushes queues/counters, clears fault/READY,
 * and sets open_pending. SAME-session OPEN is idempotent. Main must take_open,
 * stop/reset its native controller, then set_ready(true). Native startup lines
 * may be enqueued before READY. Sequence counters never wrap: open a new
 * random session before exhausting uint32_t sequences.
 *
 * COMMAND(seq=1 onward) is printable ASCII with one final LF, length 2..40.
 * Response ack acknowledges acceptance, not execution. Duplicate commands
 * are acknowledged without requeueing. Future/gapped sequence is rejected.
 * POLL(seq=len=0) and COMMAND ack the last delivered response sequence.
 * Older/repeated ACKs are harmless; unknown future ACKs reject the request.
 *
 * STOP(seq=ack=len=0) bypasses the full command queue, clears queued commands,
 * sets stop_pending, and clears READY. It preserves TX for capture draining.
 * Main must take_stop, stop its native controller, then set_ready(true).
 * A fault requests STOP, rejects commands, and remains latched until cleared.
 *
 * A response's seq/len/payload is frozen until acknowledged. Empty responses
 * use seq=len=0. Flags and command ack can change while payload remains fixed.
 */
void bwi_server_init(BwiServer* server);
bool bwi_server_receive(BwiServer* server, const uint8_t* raw, size_t length);
bool bwi_server_response(BwiServer* server, uint8_t out[BWI_FRAME_SIZE]);
bool bwi_server_take_open(BwiServer* server);
bool bwi_server_take_stop(BwiServer* server);
bool bwi_server_take_command(BwiServer* server, BwiCommand* command);
void bwi_server_set_ready(BwiServer* server, bool ready);
void bwi_server_set_fault(BwiServer* server, bool fault);
/* Input excludes CR/LF; appends LF atomically or changes nothing on failure.
 * pending/free count raw TX bytes, including the unacknowledged fragment. */
bool bwi_server_enqueue_line(BwiServer* server, const char* line, size_t length);
size_t bwi_server_free(const BwiServer* server);
size_t bwi_server_pending(const BwiServer* server);

#ifdef __cplusplus
}
#endif
#endif

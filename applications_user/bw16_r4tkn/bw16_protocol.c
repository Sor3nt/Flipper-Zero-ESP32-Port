#include "bw16_protocol.h"
#include <string.h>

static bool number(const char* s, const char* end, int min, int max, int* value) {
    bool neg = s < end && *s == '-';
    if(neg) ++s;
    if(s == end) return false;
    int n = 0;
    for(; s < end; ++s) {
        if(*s < '0' || *s > '9' || n > 1000) return false;
        n = n * 10 + *s - '0';
    }
    *value = neg ? -n : n;
    return *value >= min && *value <= max;
}

static const char* previous_comma(const char* start, const char* end) {
    while(end > start) if(*--end == ',') return end;
    return NULL;
}

static bool mac_valid(const char* s, size_t n) {
    if(n != 12 && n != 17) return false;
    for(size_t i = 0; i < n; ++i) {
        if(n == 17 && i % 3 == 2) { if(s[i] != ':') return false; }
        else if(!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                  (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}

Bw16LineKind bw16_parse(const char* line, Bw16Ap* ap) {
    /* SCAN_DONE is the transaction boundary for SCAN in the v4 image.
     * LIST_STATION_DONE alone belongs to a different listing command. */
    if(strcmp(line, "SCAN_DONE") == 0) return Bw16Done;
    if(strcmp(line, "NO SCAN RESULTS") == 0) return Bw16Empty;
    bool prefixed = strncmp(line, "list-station:", 13) == 0;
    const char* start = prefixed ? line + 13 : line;
    if(prefixed && *start == ' ') ++start;
    const char* end = start + strlen(start);
    const char* rssi_sep = previous_comma(start, end);
    const char* last_sep = rssi_sep ? previous_comma(start, rssi_sep) : NULL;
    if(!last_sep) return prefixed ? Bw16Malformed : Bw16Ignore;
    memset(ap, 0, sizeof(*ap));
    const char* channel_end = rssi_sep;
    const char* ssid_end = last_sep;
    size_t mac_len = (size_t)(rssi_sep - last_sep - 1);
    if(mac_valid(last_sep + 1, mac_len)) {
        channel_end = last_sep;
        ssid_end = previous_comma(start, channel_end);
        if(!ssid_end) return Bw16Malformed;
        memcpy(ap->mac, last_sep + 1, mac_len);
    } else if(!prefixed) {
        /* Numeric channel/RSSI delimit a candidate bare scan record. Report a
         * damaged MAC instead of turning a corrupt listing into an empty scan. */
        const char* channel_sep = previous_comma(start, last_sep);
        int channel_candidate, rssi_candidate;
        if(channel_sep && number(channel_sep + 1, last_sep, 1, 196, &channel_candidate) &&
           number(rssi_sep + 1, end, -127, 0, &rssi_candidate)) return Bw16Malformed;
        return Bw16Ignore;
    }
    int channel;
    size_t ssid_len = (size_t)(ssid_end - start);
    if(ssid_len > 32 || !number(ssid_end + 1, channel_end, 1, 196, &channel) ||
       !number(rssi_sep + 1, end, -127, 0, &ap->rssi)) return Bw16Malformed;
    /* BW16 is dual-band, not 6 GHz. Reject impossible channel identifiers. */
    if(channel > 14 && !((channel >= 32 && channel <= 144 && channel % 4 == 0) ||
                        (channel >= 149 && channel <= 177 && (channel - 149) % 4 == 0) ||
                        (channel >= 184 && channel <= 196 && channel % 4 == 0)))
        return Bw16Malformed;
    for(size_t i = 0; i < ssid_len; ++i) {
        unsigned char c = (unsigned char)start[i];
        if(c < 32 || c == 127) return Bw16Malformed;
        ap->ssid[i] = c < 127 ? (char)c : '?'; /* Canvas has an ASCII font. */
    }
    ap->channel = (unsigned)channel;
    return Bw16Record;
}

Bw16Frame bw16_frame(Bw16Line* line, uint8_t byte) {
    if(byte == '\n') {
        bool bad = line->discard;
        if(line->used && line->text[line->used - 1] == '\r') --line->used;
        line->text[line->used] = 0;
        bool nonempty = line->used != 0;
        line->used = 0;
        line->discard = false;
        return bad ? Bw16LineDropped : nonempty ? Bw16LineReady : Bw16More;
    }
    if(line->discard) return Bw16More;
    if(byte == 0 || (byte < 32 && byte != '\r') || line->used == BW16_LINE_SIZE - 1)
        line->discard = true;
    else line->text[line->used++] = (char)byte;
    return Bw16More;
}

bool bw16_scan_expired(uint32_t now, uint32_t start) {
    return (uint32_t)(now - start) >= BW16_SCAN_TIMEOUT_MS;
}

#include "bw16_capture.h"
#include <stdio.h>
#include <string.h>

static void error(Bw16Capture* c, const char* text) {
    if(!c->failed) snprintf(c->error, sizeof(c->error), "%s", text);
    c->failed = true;
}
bool bw16_capture_close(Bw16Capture* c) {
    if(c->file) {
        if(c->opened && !storage_file_sync(c->file)) error(c, "SD sync failed; partial file");
        if(c->opened && !storage_file_close(c->file)) error(c, "SD close failed; partial file");
        storage_file_free(c->file);
        c->file = NULL;
    }
    if(c->storage) { furi_record_close(RECORD_STORAGE); c->storage = NULL; }
    c->opened = false;
    c->closed = true;
    return !c->failed;
}
static int hex_digit(char ch) {
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}
static bool parse_bssid(const char* text, uint8_t out[6]) {
    if(!text) return false;
    size_t length = strlen(text);
    if(length != 12 && length != 17) return false;
    unsigned stride = length == 17 ? 3 : 2;
    for(unsigned i = 0; i < 6; ++i) {
        int high = hex_digit(text[i * stride]), low = hex_digit(text[i * stride + 1]);
        if(high < 0 || low < 0 || (stride == 3 && i < 5 && text[i * stride + 2] != ':')) return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}
bool bw16_capture_prepare(Bw16Capture* c, const char* bssid, uint32_t session, uint32_t anchor) {
    if(c->file || c->storage) return false;
    memset(c, 0, sizeof(*c));
    c->session = session;
    c->anchor = anchor;
    if(!session || !parse_bssid(bssid, c->bssid)) {
        error(c, "Invalid capture target");
        return false;
    }
    c->storage = furi_record_open(RECORD_STORAGE);
    if(!c->storage || !storage_simply_mkdir(c->storage, "/ext/apps_data") ||
       !storage_simply_mkdir(c->storage, "/ext/apps_data/bw16") ||
       !storage_simply_mkdir(c->storage, BW16_CAPTURE_DIRECTORY)) {
        error(c, "SD directory unavailable");
        bw16_capture_close(c);
        return false;
    }
    c->file = storage_file_alloc(c->storage);
    if(!c->file) { error(c, "Cannot allocate SD file"); bw16_capture_close(c); return false; }
    for(unsigned index = 1; index <= 9999; ++index) {
        snprintf(c->path, sizeof(c->path), BW16_CAPTURE_DIRECTORY "/capture_%04u.pcap", index);
        if(storage_file_open(c->file, c->path, FSAM_WRITE, FSOM_CREATE_NEW)) {
            c->opened = c->created = true;
            break;
        }
        FS_Error cause = storage_file_get_error(c->file);
        /* The storage API requires close after a failed open as well. */
        (void)storage_file_close(c->file);
        if(cause != FSE_EXIST) {
            error(c, "SD create failed");
            break;
        }
    }
    if(!c->opened) {
        c->path[0] = 0; /* No new file exists; never suggest an existing file is ours. */
        error(c, "No free capture filename");
        bw16_capture_close(c);
        return false;
    }
    uint8_t header[24];
    bwc_pcap_header(header);
    if(storage_file_write(c->file, header, sizeof(header)) != sizeof(header) ||
       !storage_file_sync(c->file)) {
        error(c, "SD header failed; partial file");
        bw16_capture_close(c);
        return false;
    }
    return true;
}
static Bw16CaptureEvent drop(Bw16Capture* c) {
    if(c->dropped != UINT32_MAX) ++c->dropped;
    return Bw16CaptureDropped;
}
static Bw16CaptureEvent packet(Bw16Capture* c, bool started) {
    BwcPacket* p = &c->packet;
    if(!started || !bwc_decode(c->line, p) || p->session != c->session ||
       p->usec > 120000000U || (c->have_sequence && (p->sequence <= c->sequence || p->usec < c->usec)) ||
       bwc_classify(p->data, p->length, c->bssid) != p->kind) return drop(c);
    uint32_t missing = c->have_sequence ? p->sequence - c->sequence - 1U : p->sequence;
    c->gaps = missing > UINT32_MAX - c->gaps ? UINT32_MAX : c->gaps + missing;
    c->sequence = p->sequence;
    c->usec = p->usec;
    c->have_sequence = true;
    if(c->failed || !c->opened) return drop(c);
    if(c->packets >= BW16_CAPTURE_PACKETS) return Bw16CaptureLimit;
    uint8_t header[16];
    uint32_t seconds = p->usec / 1000000U;
    if(seconds > UINT32_MAX - c->anchor ||
       !bwc_pcap_record_header(header, c->anchor + seconds, p->usec % 1000000U, p->length) ||
       storage_file_write(c->file, header, sizeof(header)) != sizeof(header) ||
       storage_file_write(c->file, p->data, p->length) != p->length ||
       !storage_file_sync(c->file)) {
        error(c, "SD write failed; partial file");
        return Bw16CaptureWriteError;
    }
    ++c->packets;
    if(p->kind == BwcEapol) ++c->eapol;
    return c->packets == BW16_CAPTURE_PACKETS ? Bw16CaptureLimit : Bw16CapturePacket;
}
Bw16CaptureEvent bw16_capture_byte(Bw16Capture* c, uint8_t byte, bool started) {
    if(byte == '\n') {
        bool discarded = c->discard;
        unsigned used = c->used;
        c->line[used] = 0;
        c->used = 0;
        c->discard = false;
        if(discarded) return drop(c);
        if(!used) return Bw16CaptureNone;
        if(!strncmp(c->line, "PCAP1:", 6)) return packet(c, started);
        return Bw16CaptureControl;
    }
    if(c->discard) return Bw16CaptureNone;
    if(byte == '\r') return Bw16CaptureNone;
    if(byte < 32 || byte > 126 || c->used + 1U >= sizeof(c->line)) {
        c->discard = true;
        return Bw16CaptureNone;
    }
    c->line[c->used++] = (char)byte;
    return Bw16CaptureNone;
}

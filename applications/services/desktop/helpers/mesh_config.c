#include "mesh_config.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define TAG "MeshConfig"

#define MESH_DIR     "/ext/mesh"
#define MESH_MODE    "/ext/mesh/mode.txt"
#define MESH_CLIENTS "/ext/mesh/clients.txt"
#define MESH_MASTER  "/ext/mesh/master.txt"

/* Sicherstellen dass /ext/mesh existiert; idempotent (mkdir gibt EXIST zurück,
 * was storage_simply_mkdir intern in "true" verwandelt). */
static void mesh_config_ensure_dir(Storage* storage) {
    storage_simply_mkdir(storage, MESH_DIR);
}

void mesh_mac_to_str(const uint8_t mac[MESH_MAC_LEN], char out[18]) {
    snprintf(
        out,
        18,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
}

bool mesh_mac_from_str(const char* s, uint8_t out[MESH_MAC_LEN]) {
    if(!s) return false;
    unsigned v[6];
    if(sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) !=
       6) {
        return false;
    }
    for(int i = 0; i < 6; ++i) out[i] = (uint8_t)v[i];
    return true;
}

/* ───────────────────── mode.txt ───────────────────── */

MeshMode mesh_config_load_mode(void) {
    MeshMode result = MeshModeDisabled;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* s = file_stream_alloc(storage);

    if(file_stream_open(s, MESH_MODE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        if(stream_read_line(s, line)) {
            furi_string_trim(line);
            if(furi_string_equal(line, "master")) {
                result = MeshModeMaster;
            } else if(furi_string_equal(line, "client")) {
                result = MeshModeClient;
            }
        }
        furi_string_free(line);
    }

    stream_free(s);
    furi_record_close(RECORD_STORAGE);
    return result;
}

void mesh_config_save_mode(MeshMode mode) {
    const char* text = (mode == MeshModeMaster) ? "master\n" :
                       (mode == MeshModeClient) ? "client\n" :
                                                  "disabled\n";

    Storage* storage = furi_record_open(RECORD_STORAGE);
    mesh_config_ensure_dir(storage);

    Stream* s = file_stream_alloc(storage);
    if(file_stream_open(s, MESH_MODE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        stream_write_cstring(s, text);
    } else {
        FURI_LOG_E(TAG, "save_mode: open failed");
    }
    stream_free(s);
    furi_record_close(RECORD_STORAGE);
}

/* ────────────────── peer-list shared helpers ────────────────── */

/* Parsen einer Zeile der Form "AA:BB:CC:DD:EE:FF;Name". Whitespace wird vorher
 * getrimmt. Gibt false zurück bei Format-Fehler. */
static bool mesh_parse_peer_line(FuriString* line, MeshPeer* out) {
    furi_string_trim(line);
    if(furi_string_size(line) == 0) return false;
    if(furi_string_get_char(line, 0) == '#') return false; // Kommentar erlauben

    const char* s = furi_string_get_cstr(line);
    const char* semi = strchr(s, ';');
    if(!semi || (semi - s) < 17) return false;

    char mac_buf[18] = {0};
    memcpy(mac_buf, s, 17);
    if(!mesh_mac_from_str(mac_buf, out->mac)) return false;

    const char* name = semi + 1;
    size_t n = strnlen(name, MESH_NAME_MAX);
    memcpy(out->name, name, n);
    out->name[n] = '\0';
    return true;
}

/* Generisches Read aller Peer-Zeilen aus einem File. */
static bool mesh_load_peer_file(const char* path, MeshPeer* out, size_t* out_count) {
    if(out_count) *out_count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* s = file_stream_alloc(storage);
    bool ok = true;
    size_t count = 0;

    if(file_stream_open(s, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(count < MESH_CLIENTS_MAX && stream_read_line(s, line)) {
            MeshPeer p;
            if(mesh_parse_peer_line(line, &p)) {
                if(out) out[count] = p;
                count++;
            }
        }
        furi_string_free(line);
    }
    /* Fehlende Datei ist OK (Default: leere Liste). */

    if(out_count) *out_count = count;
    stream_free(s);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static bool mesh_save_peer_file(const char* path, const MeshPeer* peers, size_t count) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    mesh_config_ensure_dir(storage);

    Stream* s = file_stream_alloc(storage);
    bool ok = false;
    if(file_stream_open(s, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = true;
        for(size_t i = 0; i < count; ++i) {
            char mac[18];
            mesh_mac_to_str(peers[i].mac, mac);
            stream_write_format(s, "%s;%s\n", mac, peers[i].name);
        }
    } else {
        FURI_LOG_E(TAG, "save_peer_file: open %s failed", path);
    }
    stream_free(s);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

/* ───────────────────── clients.txt ───────────────────── */

bool mesh_config_load_clients(MeshPeer* out, size_t* out_count) {
    return mesh_load_peer_file(MESH_CLIENTS, out, out_count);
}

bool mesh_config_save_clients(const MeshPeer* peers, size_t count) {
    return mesh_save_peer_file(MESH_CLIENTS, peers, count);
}

bool mesh_config_add_client(const MeshPeer* peer) {
    MeshPeer list[MESH_CLIENTS_MAX];
    size_t n = 0;
    mesh_config_load_clients(list, &n);

    /* Update bei vorhandener MAC, sonst append. */
    for(size_t i = 0; i < n; ++i) {
        if(memcmp(list[i].mac, peer->mac, MESH_MAC_LEN) == 0) {
            list[i] = *peer;
            return mesh_config_save_clients(list, n);
        }
    }
    if(n >= MESH_CLIENTS_MAX) {
        FURI_LOG_W(TAG, "client list full (%u)", (unsigned)n);
        return false;
    }
    list[n++] = *peer;
    return mesh_config_save_clients(list, n);
}

bool mesh_config_remove_client(const uint8_t mac[MESH_MAC_LEN]) {
    MeshPeer list[MESH_CLIENTS_MAX];
    size_t n = 0;
    mesh_config_load_clients(list, &n);

    size_t out = 0;
    bool removed = false;
    for(size_t i = 0; i < n; ++i) {
        if(memcmp(list[i].mac, mac, MESH_MAC_LEN) == 0) {
            removed = true;
            continue;
        }
        list[out++] = list[i];
    }
    if(!removed) return true; // nichts zu tun
    return mesh_config_save_clients(list, out);
}

/* ───────────────────── master.txt ───────────────────── */

bool mesh_config_load_master(MeshPeer* out) {
    MeshPeer tmp;
    size_t n = 0;
    if(!mesh_load_peer_file(MESH_MASTER, &tmp, &n)) return false;
    if(n == 0) return false;
    if(out) *out = tmp;
    return true;
}

bool mesh_config_save_master(const MeshPeer* peer) {
    return mesh_save_peer_file(MESH_MASTER, peer, 1);
}

void mesh_config_clear_master(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, MESH_MASTER);
    furi_record_close(RECORD_STORAGE);
}

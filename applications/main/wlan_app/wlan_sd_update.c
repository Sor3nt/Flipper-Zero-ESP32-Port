#include "wlan_sd_update.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>

#define SD_UPDATE_TAG "WlanSdUpdate"
// sdcard/-Ordner wird unter dieser Basis gespiegelt veröffentlicht.
#define SD_UPDATE_BASE_URL "https://sor3nt.github.io/release/t-embed/latest"
#define SD_UPDATE_VERSION_URL SD_UPDATE_BASE_URL "/version.txt"
#define SD_UPDATE_FILES_URL SD_UPDATE_BASE_URL "/files.txt"
#define SD_UPDATE_LOCAL_VERSION "/ext/version.txt"
#define SD_UPDATE_DEST_ROOT "/ext"
#define SD_UPDATE_MAX_MANIFEST (4u * 1024u * 1024u)
#define SD_UPDATE_CHUNK 8192
#define SD_UPDATE_LOCAL_MANIFEST "/ext/files.txt"
// Anzahl Versuche pro Datei bei Read-Timeout/Verbindungsabbruch. Jeder Retry
// setzt per HTTP-Range an der bereits geschriebenen Byte-Position fort.
#define SD_UPDATE_MAX_RETRY 4

static void* sd_malloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

struct WlanSdUpdate {
    TaskHandle_t task;
    volatile WlanSdUpdatePhase phase;
    volatile uint8_t percent;
    volatile bool cancel;
    volatile bool running;
    volatile uint32_t speed_kbps;
    volatile uint32_t done_files;
    volatile uint32_t total_files;
    char current_file[64];
    char err[64];
};

static void sd_update_set_file(WlanSdUpdate* u, const char* name) {
    strncpy(u->current_file, name, sizeof(u->current_file) - 1);
    u->current_file[sizeof(u->current_file) - 1] = '\0';
}

static void sd_update_fail(WlanSdUpdate* u, const char* msg) {
    strncpy(u->err, msg, sizeof(u->err) - 1);
    u->err[sizeof(u->err) - 1] = '\0';
    u->phase = WlanSdUpdateError;
    FURI_LOG_E(SD_UPDATE_TAG, "%s", msg);
}

// Schneidet führende/abschließende Whitespaces (inkl. \r\n) in-place ab.
static void sd_update_trim(char* s) {
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                    s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while(s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n') {
        start++;
    }
    if(start) memmove(s, s + start, strlen(s + start) + 1);
}

static void sd_update_http_cfg(esp_http_client_config_t* cfg, const char* url) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 40000;
    cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
    // SSL-Verifikation deaktiviert (kein CA gesetzt; benötigt
    // CONFIG_ESP_TLS_INSECURE / SKIP_SERVER_CERT_VERIFY).
    cfg->skip_cert_common_name_check = true;
    cfg->crt_bundle_attach = NULL;
    cfg->use_global_ca_store = false;
    cfg->buffer_size = SD_UPDATE_CHUNK;
    cfg->buffer_size_tx = 1024;
    // Verbindung/TLS-Session über mehrere Dateien wiederverwenden, sonst
    // zahlt jede Datei einen kompletten TLS-Handshake (sehr langsam).
    cfg->keep_alive_enable = true;
}

// Lädt eine kleine Text-Resource synchron in out (nul-terminiert).
static bool sd_update_http_get_text(const char* url, char* out, size_t out_sz) {
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return false;

    bool ok = false;
    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) == 200) {
            size_t len = 0;
            while(len + 1 < out_sz) {
                int r = esp_http_client_read(client, out + len, out_sz - 1 - len);
                if(r < 0) {
                    len = 0;
                    break;
                }
                if(r == 0) break;
                len += (size_t)r;
            }
            out[len] = '\0';
            ok = len > 0;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// Lädt das (Text-)Manifest in einen malloc-Puffer. Caller frees.
static char* sd_update_http_get_alloc(const char* url, size_t* out_len) {
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return NULL;

    char* buf = NULL;
    size_t cap = 0, len = 0;
    bool ok = false;

    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) == 200) {
            ok = true;
            while(true) {
                if(len + 1 >= cap) {
                    size_t ncap = cap ? cap * 2 : 32768;
                    if(ncap > SD_UPDATE_MAX_MANIFEST) {
                        ok = false;
                        break;
                    }
                    char* nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
                    if(!nb) {
                        ok = false;
                        break;
                    }
                    buf = nb;
                    cap = ncap;
                }
                int r = esp_http_client_read(client, buf + len, cap - 1 - len);
                if(r < 0) {
                    ok = false;
                    break;
                }
                if(r == 0) break;
                len += (size_t)r;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if(!ok || len == 0) {
        if(buf) free(buf);
        return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static bool sd_update_read_local_version(char* out, size_t out_sz) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    bool ok = false;
    if(storage_file_open(f, SD_UPDATE_LOCAL_VERSION, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t r = storage_file_read(f, out, out_sz - 1);
        out[r] = '\0';
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// true → lokale version.txt existiert und ist identisch mit der Remote-Version.
static bool sd_update_is_up_to_date(void) {
    char remote[64];
    char local[64];
    if(!sd_update_http_get_text(SD_UPDATE_VERSION_URL, remote, sizeof(remote))) {
        return false;
    }
    if(!sd_update_read_local_version(local, sizeof(local))) {
        return false;
    }
    sd_update_trim(remote);
    sd_update_trim(local);
    return remote[0] != '\0' && strcmp(remote, local) == 0;
}

// Legt /ext/a/b rekursiv an (ohne den finalen Dateinamen).
static void sd_update_mkdirs(Storage* storage, const char* path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for(char* p = tmp + 1; *p; ++p) {
        if(*p == '/') {
            *p = '\0';
            storage_common_mkdir(storage, tmp);
            *p = '/';
        }
    }
}

// Ein einzelner Download-Versuch über den (wiederverwendeten) Client.
//   *resume_from: bereits lokal vorhandene Bytes; wird per HTTP-Range
//                 fortgesetzt und auf den neuen Stand mitgeführt.
//   *complete:    true, wenn der Stream vollständig bis zum Ende gelesen wurde.
// Rückgabe true nur bei vollständigem Empfang; bei Read-Timeout/Abbruch false,
// wobei *resume_from den letzten geschriebenen Stand behält (für den Retry).
static bool sd_update_download_attempt(
    WlanSdUpdate* u,
    esp_http_client_handle_t client,
    Storage* storage,
    const char* url,
    const char* dest,
    uint32_t* resume_from,
    bool* complete) {
    *complete = false;
    if(esp_http_client_set_url(client, url) != ESP_OK) return false;

    if(*resume_from > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)*resume_from);
        esp_http_client_set_header(client, "Range", range);
    } else {
        // Stale Range-Header vom vorherigen Versuch am Reuse-Client entfernen.
        esp_http_client_delete_header(client, "Range");
    }

    if(esp_http_client_open(client, 0) != ESP_OK) return false;

    bool ok = false;
    File* f = NULL;
    uint8_t* chunk = NULL;

    do {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        // 206 = Server akzeptiert Range (Resume). 200 = voller Inhalt — auch
        // wenn wir Range gefordert haben (Server ignoriert es) → von vorn.
        bool resumed = (status == 206);
        if(status != 200 && status != 206) break;
        if(*resume_from > 0 && !resumed) *resume_from = 0;

        f = storage_file_alloc(storage);
        if(resumed && *resume_from > 0) {
            if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_OPEN_EXISTING)) break;
            if(!storage_file_seek(f, *resume_from, true)) break;
        } else {
            if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;
        }

        chunk = malloc(SD_UPDATE_CHUNK);
        if(!chunk) break;

        ok = true;
        uint32_t t0 = furi_get_tick();
        uint32_t total = *resume_from; // gesamt (für Resume-Offset)
        uint32_t session = 0;          // nur dieser Versuch (für Speed)
        while(!u->cancel) {
            int r = esp_http_client_read(client, (char*)chunk, SD_UPDATE_CHUNK);
            if(r < 0) {
                ok = false; // Timeout/Reset → Versuch gescheitert, Retry folgt
                break;
            }
            if(r == 0) {
                *complete = esp_http_client_is_complete_data_received(client);
                break;
            }
            if(storage_file_write(f, chunk, (size_t)r) != (size_t)r) {
                ok = false;
                break;
            }
            total += (uint32_t)r;
            session += (uint32_t)r;
            *resume_from = total;
            uint32_t dt = furi_get_tick() - t0;
            if(dt >= 200) {
                u->speed_kbps = (uint32_t)((uint64_t)session * 1000u / 1024u / dt);
            }
        }
        if(u->cancel) ok = false;
    } while(0);

    if(chunk) free(chunk);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    esp_http_client_close(client);

    return ok && *complete;
}

// Lädt eine Einzeldatei nach dest, mit bis zu SD_UPDATE_MAX_RETRY Versuchen.
// Bei Read-Timeout/Verbindungsabbruch wird per HTTP-Range an der bereits
// geschriebenen Position fortgesetzt (kein kompletter Neu-Download).
static bool sd_update_download_file(
    WlanSdUpdate* u,
    esp_http_client_handle_t client,
    Storage* storage,
    const char* url,
    const char* dest) {
    sd_update_mkdirs(storage, dest);

    uint32_t resume_from = 0;
    bool complete = false;

    for(int attempt = 0; attempt < SD_UPDATE_MAX_RETRY && !u->cancel; attempt++) {
        if(attempt > 0) {
            FURI_LOG_W(
                SD_UPDATE_TAG,
                "retry %d/%d %s @ %lu",
                attempt,
                SD_UPDATE_MAX_RETRY - 1,
                dest,
                (unsigned long)resume_from);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if(sd_update_download_attempt(
               u, client, storage, url, dest, &resume_from, &complete)) {
            return true;
        }
        if(u->cancel) return false;
    }
    return false;
}

// Path-Traversal-Schutz; baut /ext/<rel>.
static bool sd_update_safe_dest(const char* rel, char* out, size_t out_sz) {
    while(*rel == '/') rel++;
    if(!*rel) return false;
    if(strstr(rel, "..")) return false;
    int n = snprintf(out, out_sz, "%s/%s", SD_UPDATE_DEST_ROOT, rel);
    return n > 0 && (size_t)n < out_sz;
}

static uint32_t sd_update_count_lines(const char* s) {
    uint32_t n = 0;
    for(; *s; ++s)
        if(*s == '\n') n++;
    return n ? n : 1;
}

// Ein Manifest-Eintrag (Zeiger zeigen in den jeweiligen Puffer).
typedef struct {
    const char* path; // nul-terminiert
    const char* sha;  // 64 Zeichen, nul-terminiert
} SdManifestEntry;

// Parst "<64 sha> <size> <pfad>" aus einer (mutierbaren) Zeile. Liefert false
// bei Formatfehler. sha/pfad werden in-place nul-terminiert.
static bool sd_update_parse_line(
    char* line, const char** sha, uint64_t* size, const char** path) {
    size_t ll = strlen(line);
    while(ll && (line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = '\0';
    if(ll < 67 || line[64] != ' ') return false;
    line[64] = '\0';
    *sha = line;
    char* p = line + 65;
    while(*p == ' ') p++;
    uint64_t sz = 0;
    while(*p >= '0' && *p <= '9') sz = sz * 10u + (uint64_t)(*p++ - '0');
    if(*p != ' ') return false;
    if(size) *size = sz;
    p++;
    while(*p == ' ') p++;
    if(!*p) return false;
    *path = p;
    return true;
}

static int sd_manifest_cmp(const void* a, const void* b) {
    return strcmp(((const SdManifestEntry*)a)->path, ((const SdManifestEntry*)b)->path);
}

// Lädt /ext/files.txt und baut ein sortiertes Array. *out_buf muss vom Caller
// freigegeben werden. Liefert false wenn keine lokale Manifest-Datei da ist.
static bool sd_update_load_local_manifest(
    Storage* storage, char** out_buf, SdManifestEntry** out_entries, uint32_t* out_count) {
    *out_buf = NULL;
    *out_entries = NULL;
    *out_count = 0;

    FileInfo fi;
    if(storage_common_stat(storage, SD_UPDATE_LOCAL_MANIFEST, &fi) != FSE_OK) return false;
    if(fi.size == 0 || fi.size > SD_UPDATE_MAX_MANIFEST) return false;

    char* buf = sd_malloc((size_t)fi.size + 1);
    if(!buf) return false;

    File* f = storage_file_alloc(storage);
    if(!storage_file_open(f, SD_UPDATE_LOCAL_MANIFEST, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        free(buf);
        return false;
    }
    size_t rd = storage_file_read(f, buf, (size_t)fi.size);
    storage_file_close(f);
    storage_file_free(f);
    buf[rd] = '\0';

    uint32_t cap = sd_update_count_lines(buf);
    SdManifestEntry* arr = sd_malloc(sizeof(SdManifestEntry) * cap);
    if(!arr) {
        free(buf);
        return false;
    }

    uint32_t n = 0;
    char* save = NULL;
    for(char* line = strtok_r(buf, "\n", &save); line && n < cap;
        line = strtok_r(NULL, "\n", &save)) {
        const char *sha, *path;
        if(sd_update_parse_line(line, &sha, NULL, &path)) {
            arr[n].sha = sha;
            arr[n].path = path;
            n++;
        }
    }
    qsort(arr, n, sizeof(SdManifestEntry), sd_manifest_cmp);

    *out_buf = buf;
    *out_entries = arr;
    *out_count = n;
    return true;
}

static const char* sd_manifest_lookup(
    SdManifestEntry* arr, uint32_t n, const char* path) {
    SdManifestEntry key = {.path = path, .sha = NULL};
    SdManifestEntry* hit = bsearch(&key, arr, n, sizeof(SdManifestEntry), sd_manifest_cmp);
    return hit ? hit->sha : NULL;
}

static void sd_update_save_manifest(Storage* storage, const char* data, size_t len) {
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, SD_UPDATE_LOCAL_MANIFEST, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, data, len);
        storage_file_close(f);
    }
    storage_file_free(f);
}

// Delta-Sync: vergleicht das (frische) Remote-Manifest mit dem zuletzt
// gespeicherten lokalen /ext/files.txt; lädt nur neue/geänderte Dateien.
static bool sd_update_sync(WlanSdUpdate* u, const char* manifest, size_t mlen) {
    uint32_t total = sd_update_count_lines(manifest);
    uint32_t done = 0;
    u->total_files = total;
    u->done_files = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SD_UPDATE_DEST_ROOT);

    char* lbuf = NULL;
    SdManifestEntry* lentries = NULL;
    uint32_t lcount = 0;
    bool have_local =
        sd_update_load_local_manifest(storage, &lbuf, &lentries, &lcount);

    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, SD_UPDATE_BASE_URL "/files.txt");
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    bool ok = (client != NULL);
    const char* cur = manifest;
    char line[300];

    while(ok && *cur) {
        if(u->cancel) {
            ok = false;
            break;
        }
        const char* nl = strchr(cur, '\n');
        size_t ln = nl ? (size_t)(nl - cur) : strlen(cur);
        if(ln >= sizeof(line)) ln = sizeof(line) - 1;
        memcpy(line, cur, ln);
        line[ln] = '\0';
        cur = nl ? nl + 1 : cur + strlen(cur);

        done++;
        u->done_files = done;
        u->percent = (uint8_t)((uint64_t)done * 100u / total);

        const char *want_sha, *rel;
        uint64_t want_size = 0;
        if(!sd_update_parse_line(line, &want_sha, &want_size, &rel)) continue;

        char dest[256];
        if(!sd_update_safe_dest(rel, dest, sizeof(dest))) continue;

        bool need = true;
        FileInfo fi;
        bool exists = storage_common_stat(storage, dest, &fi) == FSE_OK;
        const char* lsha = have_local ? sd_manifest_lookup(lentries, lcount, rel) : NULL;
        if(lsha && exists && strcmp(lsha, want_sha) == 0) {
            need = false; // laut lokalem Manifest unverändert
        } else if(!have_local && exists && fi.size == want_size) {
            // Erstlauf ohne lokales Manifest: vorhandene Datei mit passender
            // Größe als aktuell annehmen (kein Hashing → schnell). Nach dem
            // Lauf wird das Manifest persistiert → danach exakter SHA-Diff.
            need = false;
        }

        if(!need) continue;

        sd_update_set_file(u, rel);
        u->speed_kbps = 0;

        char url[400];
        snprintf(url, sizeof(url), "%s/%s", SD_UPDATE_BASE_URL, rel);
        FURI_LOG_I(SD_UPDATE_TAG, "fetch %s", rel);
        if(!sd_update_download_file(u, client, storage, url, dest)) {
            if(u->cancel) {
                ok = false;
                break;
            }
            char m[64];
            snprintf(m, sizeof(m), "Download failed: %.32s", rel);
            sd_update_fail(u, m);
            ok = false;
            break;
        }
    }

    if(client) esp_http_client_cleanup(client);
    if(lentries) free(lentries);
    if(lbuf) free(lbuf);

    // Nur bei vollständigem Erfolg das Manifest persistieren (sonst beim
    // nächsten Lauf erneut diffen).
    if(ok && !u->cancel) {
        sd_update_save_manifest(storage, manifest, mlen);
    }

    furi_record_close(RECORD_STORAGE);
    return ok;
}

// ---------------------------------------------------------------------------
// Worker-Task
// ---------------------------------------------------------------------------

static void sd_update_finish(WlanSdUpdate* u) {
    u->running = false;
    u->task = NULL;
    vTaskDelete(NULL);
}

static void sd_update_task(void* arg) {
    WlanSdUpdate* u = arg;

    u->phase = WlanSdUpdateChecking;
    u->percent = 0;

    if(!u->cancel && sd_update_is_up_to_date()) {
        u->phase = WlanSdUpdateUpToDate;
        sd_update_finish(u);
        return;
    }
    if(u->cancel) {
        u->phase = WlanSdUpdateIdle;
        sd_update_finish(u);
        return;
    }

    u->phase = WlanSdUpdateDownloading;
    u->percent = 0;

    size_t mlen = 0;
    char* manifest = sd_update_http_get_alloc(SD_UPDATE_FILES_URL, &mlen);
    if(!manifest) {
        if(u->cancel) {
            u->phase = WlanSdUpdateIdle;
        } else {
            sd_update_fail(u, "files.txt fetch failed");
        }
        sd_update_finish(u);
        return;
    }

    bool ok = sd_update_sync(u, manifest, mlen);
    free(manifest);

    if(ok) {
        u->percent = 100;
        u->phase = WlanSdUpdateDone;
    } else if(u->cancel && u->phase != WlanSdUpdateError) {
        u->phase = WlanSdUpdateIdle;
    }

    sd_update_finish(u);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

WlanSdUpdate* wlan_sd_update_alloc(void) {
    WlanSdUpdate* u = malloc(sizeof(WlanSdUpdate));
    u->task = NULL;
    u->phase = WlanSdUpdateIdle;
    u->percent = 0;
    u->cancel = false;
    u->running = false;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    sd_update_set_file(u, "version.txt");
    return u;
}

void wlan_sd_update_free(WlanSdUpdate* u) {
    if(!u) return;
    wlan_sd_update_cancel(u);
    free(u);
}

void wlan_sd_update_start(WlanSdUpdate* u) {
    if(u->running) return;
    u->cancel = false;
    u->percent = 0;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    sd_update_set_file(u, "version.txt");
    u->phase = WlanSdUpdateChecking;
    u->running = true;
    if(xTaskCreate(sd_update_task, "WlanSdUpd", 8192, u, 4, &u->task) != pdPASS) {
        u->running = false;
        u->task = NULL;
        sd_update_fail(u, "Task spawn failed");
    }
}

void wlan_sd_update_cancel(WlanSdUpdate* u) {
    if(!u->running) return;
    u->cancel = true;
    for(int i = 0; i < 200 && u->running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

WlanSdUpdatePhase wlan_sd_update_get_phase(const WlanSdUpdate* u) {
    return u->phase;
}

uint8_t wlan_sd_update_get_percent(const WlanSdUpdate* u) {
    return u->percent;
}

const char* wlan_sd_update_get_error(const WlanSdUpdate* u) {
    return u->err;
}

bool wlan_sd_update_is_running(const WlanSdUpdate* u) {
    return u->running;
}

const char* wlan_sd_update_get_current_file(const WlanSdUpdate* u) {
    return u->current_file;
}

uint32_t wlan_sd_update_get_speed_kbps(const WlanSdUpdate* u) {
    return u->speed_kbps;
}

uint32_t wlan_sd_update_get_done(const WlanSdUpdate* u) {
    return u->done_files;
}

uint32_t wlan_sd_update_get_total(const WlanSdUpdate* u) {
    return u->total_files;
}

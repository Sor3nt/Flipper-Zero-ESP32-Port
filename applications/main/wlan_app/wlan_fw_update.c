#include "wlan_fw_update.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/fw_version.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_client.h>
#include <fw_ota/fw_ota.h>

#define FW_UPDATE_TAG "WlanFwUpdate"
// Muss mit dem Release-Layout übereinstimmen (siehe auch wlan_sd_update.c).
#define FW_BASE_URL "https://sor3nt.github.io/release/t-embed/latest"
#define FW_VERSION_URL FW_BASE_URL "/version.txt"
#define FW_BIN_URL FW_BASE_URL "/furi_esp32.bin"
// FW-Marker (/ext/.fw_version) und Staging-Pfad (/ext/update/furi_esp32.bin)
// kommen aus fw_ota.h — derselbe Ort, den auch der RPC-/qT-Embed-Updater nutzt.
// Autoritativ fuer "FW aktuell?" ist FURI_ESP32_VERSION (toolbox/fw_version.h),
// der Marker ist nur Fallback/Info (siehe fw_check_task).
#define FW_LOCAL_DIR FW_OTA_STAGE_DIR
#define FW_LOCAL_BIN FW_OTA_STAGE_BIN
#define FW_CHUNK 8192
#define FW_MAX_RETRY 4

struct WlanFwUpdate {
    TaskHandle_t task;
    volatile FwUpdatePhase phase;
    volatile uint8_t percent;
    volatile bool cancel;
    volatile bool running;
    volatile uint32_t speed_kbps;
    volatile uint32_t bytes_done;
    volatile uint32_t bytes_total;
    char remote_version[32];
    char err[64];
};

static void fw_fail(WlanFwUpdate* u, const char* msg) {
    strncpy(u->err, msg, sizeof(u->err) - 1);
    u->err[sizeof(u->err) - 1] = '\0';
    u->phase = FwUpdateError;
    FURI_LOG_E(FW_UPDATE_TAG, "%s", msg);
}

static void fw_trim(char* s) {
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                    s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while(s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') {
        start++;
    }
    if(start) memmove(s, s + start, strlen(s + start) + 1);
}

static void fw_http_cfg(esp_http_client_config_t* cfg, const char* url) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 40000;
    cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
    // SSL-Verifikation deaktiviert (kein CA gesetzt; benötigt
    // CONFIG_ESP_TLS_INSECURE / SKIP_SERVER_CERT_VERIFY, wie beim SD-Update).
    cfg->skip_cert_common_name_check = true;
    cfg->crt_bundle_attach = NULL;
    cfg->use_global_ca_store = false;
    cfg->buffer_size = FW_CHUNK;
    cfg->buffer_size_tx = 1024;
    cfg->keep_alive_enable = true;
}

// Lädt eine kleine Text-Resource synchron in out (nul-terminiert).
static bool fw_http_get_text(const char* url, char* out, size_t out_sz) {
    esp_http_client_config_t cfg;
    fw_http_cfg(&cfg, url);
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

// ---------------------------------------------------------------------------
// Check-Task
// ---------------------------------------------------------------------------

static void fw_finish(WlanFwUpdate* u) {
    u->running = false;
    u->task = NULL;
    vTaskDelete(NULL);
}

static void fw_check_task(void* arg) {
    WlanFwUpdate* u = arg;
    u->phase = FwUpdateChecking;

    char remote[32] = {0};
    char local[32] = {0};

    if(u->cancel) {
        u->phase = FwUpdateIdle;
        fw_finish(u);
        return;
    }
    if(!fw_http_get_text(FW_VERSION_URL, remote, sizeof(remote))) {
        if(u->cancel) {
            u->phase = FwUpdateIdle;
        } else {
            fw_fail(u, "version.txt fetch failed");
        }
        fw_finish(u);
        return;
    }
    fw_trim(remote);
    strncpy(u->remote_version, remote, sizeof(u->remote_version) - 1);
    u->remote_version[sizeof(u->remote_version) - 1] = '\0';

    // Autoritativ ist die einkompilierte Version der laufenden Firmware — die
    // stimmt nach jedem OTA automatisch und ueberlebt einen SD-Kartentausch.
    // Der Marker auf der SD ist nur Fallback: er faengt den Update-Loop ab,
    // falls ein Release-Binary eine abweichende einkompilierte Version traegt.
    bool have_local = fw_ota_marker_read(local, sizeof(local));

    bool up_to_date = remote[0] != '\0' && (strcmp(remote, FURI_ESP32_VERSION) == 0 ||
                                            (have_local && strcmp(remote, local) == 0));

    u->phase = up_to_date ? FwUpdateUpToDate : FwUpdateAvailable;
    fw_finish(u);
}

// ---------------------------------------------------------------------------
// Download-Task
// ---------------------------------------------------------------------------

static void fw_mkdirs(Storage* storage) {
    storage_common_mkdir(storage, FW_LOCAL_DIR);
}

// Ein Download-Versuch (from scratch). true nur bei vollständigem Empfang.
static bool fw_download_attempt(WlanFwUpdate* u, const char* url, const char* dest) {
    esp_http_client_config_t cfg;
    fw_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = NULL;
    uint8_t* chunk = NULL;
    bool ok = false;

    do {
        if(esp_http_client_open(client, 0) != ESP_OK) break;
        int64_t total = esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) != 200) break;
        u->bytes_total = (total > 0) ? (uint32_t)total : 0;

        fw_mkdirs(storage);
        f = storage_file_alloc(storage);
        if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;

        chunk = malloc(FW_CHUNK);
        if(!chunk) break;

        ok = true;
        uint32_t written = 0;
        uint32_t t0 = furi_get_tick();
        while(!u->cancel) {
            int r = esp_http_client_read(client, (char*)chunk, FW_CHUNK);
            if(r < 0) {
                ok = false; // Timeout/Reset → Versuch gescheitert
                break;
            }
            if(r == 0) {
                ok = esp_http_client_is_complete_data_received(client);
                break;
            }
            if(storage_file_write(f, chunk, (size_t)r) != (size_t)r) {
                ok = false;
                break;
            }
            written += (uint32_t)r;
            u->bytes_done = written;
            if(u->bytes_total) {
                u->percent = (uint8_t)((uint64_t)written * 100u / u->bytes_total);
            }
            uint32_t dt = furi_get_tick() - t0;
            if(dt >= 200) {
                u->speed_kbps = (uint32_t)((uint64_t)written * 1000u / 1024u / dt);
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
    esp_http_client_cleanup(client);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void fw_download_task(void* arg) {
    WlanFwUpdate* u = arg;
    u->phase = FwUpdateDownloading;
    u->percent = 0;
    u->bytes_done = 0;
    u->bytes_total = 0;
    u->speed_kbps = 0;

    bool ok = false;
    for(int attempt = 0; attempt < FW_MAX_RETRY && !u->cancel; attempt++) {
        if(attempt > 0) {
            FURI_LOG_W(FW_UPDATE_TAG, "download retry %d/%d", attempt, FW_MAX_RETRY - 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if(fw_download_attempt(u, FW_BIN_URL, FW_LOCAL_BIN)) {
            ok = true;
            break;
        }
        if(u->cancel) break;
    }

    if(ok) {
        u->percent = 100;
        u->phase = FwUpdateDownloaded;
    } else if(u->cancel && u->phase != FwUpdateError) {
        u->phase = FwUpdateIdle;
    } else if(u->phase != FwUpdateError) {
        fw_fail(u, "download failed");
    }
    fw_finish(u);
}

// ---------------------------------------------------------------------------
// Flash-Task (esp_ota) — Kern liegt in components/fw_ota (geteilt mit dem
// RPC-/qT-Embed-Updater). Laeuft hier in einem xTaskCreate-Task mit internem
// DRAM-Stack (siehe fw_ota.h).
// ---------------------------------------------------------------------------

static void fw_flash_progress(uint32_t done, uint32_t total, void* ctx) {
    WlanFwUpdate* u = ctx;
    u->bytes_total = total;
    u->bytes_done = done;
    u->percent = total ? (uint8_t)((uint64_t)done * 100u / total) : 0;
}

static void fw_flash_task(void* arg) {
    WlanFwUpdate* u = arg;
    u->phase = FwUpdateFlashing;
    u->percent = 0;
    u->bytes_done = 0;
    u->speed_kbps = 0;

    char err[64] = {0};
    if(fw_ota_flash_file(FW_LOCAL_BIN, fw_flash_progress, u, err, sizeof(err))) {
        // FW-Marker /ext/.fw_version auf die neue Version setzen, damit nach dem
        // Reboot die FW als aktuell erkannt wird (SD-Version bleibt getrennt).
        if(u->remote_version[0]) {
            fw_ota_marker_write(u->remote_version);
        }
        u->percent = 100;
        u->phase = FwUpdateDone;
    } else {
        fw_fail(u, err[0] ? err : "flash failed");
    }
    fw_finish(u);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void wlan_fw_update_sync_marker(void) {
    fw_ota_marker_sync();
}

WlanFwUpdate* wlan_fw_update_alloc(void) {
    WlanFwUpdate* u = malloc(sizeof(WlanFwUpdate));
    u->task = NULL;
    u->phase = FwUpdateIdle;
    u->percent = 0;
    u->cancel = false;
    u->running = false;
    u->speed_kbps = 0;
    u->bytes_done = 0;
    u->bytes_total = 0;
    u->remote_version[0] = '\0';
    u->err[0] = '\0';
    return u;
}

void wlan_fw_update_free(WlanFwUpdate* u) {
    if(!u) return;
    wlan_fw_update_cancel(u);
    free(u);
}

static void fw_start_task(WlanFwUpdate* u, TaskFunction_t fn, const char* name) {
    if(u->running) return;
    u->cancel = false;
    u->err[0] = '\0';
    u->running = true;
    if(xTaskCreate(fn, name, 8192, u, 4, &u->task) != pdPASS) {
        u->running = false;
        u->task = NULL;
        fw_fail(u, "Task spawn failed");
    }
}

void wlan_fw_update_check_start(WlanFwUpdate* u) {
    u->percent = 0;
    fw_start_task(u, fw_check_task, "WlanFwChk");
}

void wlan_fw_update_download_start(WlanFwUpdate* u) {
    u->percent = 0;
    u->bytes_done = 0;
    u->bytes_total = 0;
    u->speed_kbps = 0;
    fw_start_task(u, fw_download_task, "WlanFwDl");
}

void wlan_fw_update_flash_start(WlanFwUpdate* u) {
    u->percent = 0;
    u->bytes_done = 0;
    u->bytes_total = 0;
    fw_start_task(u, fw_flash_task, "WlanFwOta");
}

void wlan_fw_update_cancel(WlanFwUpdate* u) {
    if(!u->running) return;
    u->cancel = true;
    for(int i = 0; i < 200 && u->running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

FwUpdatePhase wlan_fw_update_get_phase(const WlanFwUpdate* u) {
    return u->phase;
}

uint8_t wlan_fw_update_get_percent(const WlanFwUpdate* u) {
    return u->percent;
}

const char* wlan_fw_update_get_error(const WlanFwUpdate* u) {
    return u->err;
}

const char* wlan_fw_update_get_remote_version(const WlanFwUpdate* u) {
    return u->remote_version;
}

bool wlan_fw_update_is_running(const WlanFwUpdate* u) {
    return u->running;
}

uint32_t wlan_fw_update_get_speed_kbps(const WlanFwUpdate* u) {
    return u->speed_kbps;
}

uint32_t wlan_fw_update_get_bytes_done(const WlanFwUpdate* u) {
    return u->bytes_done;
}

uint32_t wlan_fw_update_get_bytes_total(const WlanFwUpdate* u) {
    return u->bytes_total;
}

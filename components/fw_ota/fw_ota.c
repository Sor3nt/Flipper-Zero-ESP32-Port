#include "fw_ota.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/fw_version.h>

#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#define FW_OTA_HAVE_FORCE_DOWNLOAD_BOOT 1
#else
#define FW_OTA_HAVE_FORCE_DOWNLOAD_BOOT 0
#endif

#define TAG "FwOta"

// Chunk-Groesse fuer den Flash-Pfad; der Puffer liegt zwingend in internem DRAM.
#define FW_OTA_CHUNK 8192

// Magic fuer das RTC-NOINIT-Flag (RTC_NOINIT bleibt ueber esp_restart erhalten,
// ist nach Power-On aber Zufall — daher Magic statt bool).
#define FW_OTA_RESUME_QFLIPPER_MAGIC 0x51464C50u /* 'QFLP' */
static RTC_NOINIT_ATTR uint32_t s_resume_qflipper;

static void fw_ota_set_err(char* err, size_t err_size, const char* msg) {
    if(!err || !err_size) return;
    strncpy(err, msg, err_size - 1);
    err[err_size - 1] = '\0';
}

static void fw_ota_trim(char* s) {
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while(s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') {
        start++;
    }
    if(start) memmove(s, s + start, strlen(s + start) + 1);
}

bool fw_ota_is_supported(void) {
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

const char* fw_ota_running_partition_label(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running ? running->label : "?";
}

const char* fw_ota_image_status_str(FwOtaImageStatus status) {
    switch(status) {
    case FwOtaImageOk:
        return "ok";
    case FwOtaImageFileNotFound:
        return "firmware file missing";
    case FwOtaImageNotAnImage:
        return "not an ESP32 image";
    case FwOtaImageChipMismatch:
        return "image built for another chip";
    case FwOtaImageWrongProject:
        return "not a furi_esp32 image";
    case FwOtaImageReadError:
    default:
        return "read error";
    }
}

FwOtaImageStatus fw_ota_inspect_image(const char* path, FwOtaImageInfo* out) {
    if(out) memset(out, 0, sizeof(*out));
    if(!path || !path[0]) return FwOtaImageFileNotFound;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FwOtaImageStatus status = FwOtaImageReadError;

    FileInfo fi;
    if(storage_common_stat(storage, path, &fi) != FSE_OK || fi.size == 0) {
        furi_record_close(RECORD_STORAGE);
        return FwOtaImageFileNotFound;
    }

    // Image-Header (24 B) + erster Segment-Header (8 B) + App-Descriptor.
    uint8_t hdr[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                sizeof(esp_app_desc_t)];
    File* f = storage_file_alloc(storage);
    do {
        if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) break;
        size_t r = storage_file_read(f, hdr, sizeof(hdr));
        if(r != sizeof(hdr)) {
            status = (r >= sizeof(esp_image_header_t) && hdr[0] != ESP_IMAGE_HEADER_MAGIC) ?
                         FwOtaImageNotAnImage :
                         FwOtaImageReadError;
            break;
        }

        const esp_image_header_t* ih = (const esp_image_header_t*)hdr;
        if(ih->magic != ESP_IMAGE_HEADER_MAGIC) {
            status = FwOtaImageNotAnImage;
            break;
        }

        const esp_app_desc_t* ad =
            (const esp_app_desc_t*)(hdr + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
        bool have_desc = ad->magic_word == ESP_APP_DESC_MAGIC_WORD;

        if(out) {
            out->chip_id = (uint16_t)ih->chip_id;
            out->size = (uint32_t)fi.size;
            if(have_desc) {
                strncpy(out->version, ad->version, sizeof(out->version) - 1);
                strncpy(out->project_name, ad->project_name, sizeof(out->project_name) - 1);
                strncpy(out->date, ad->date, sizeof(out->date) - 1);
                strncpy(out->time, ad->time, sizeof(out->time) - 1);
            }
        }

        if(ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            status = FwOtaImageChipMismatch;
            break;
        }
        if(!have_desc || strncmp(ad->project_name, FW_OTA_PROJECT_NAME, sizeof(ad->project_name)) != 0) {
            status = FwOtaImageWrongProject;
            break;
        }
        status = FwOtaImageOk;
    } while(0);

    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return status;
}

bool fw_ota_flash_file(
    const char* path,
    FwOtaProgressCb progress,
    void* progress_ctx,
    char* err,
    size_t err_size) {
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    if(!next) {
        fw_ota_set_err(err, err_size, "OTA not supported (no ota slot)");
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo fi;
    if(storage_common_stat(storage, path, &fi) != FSE_OK || fi.size == 0) {
        furi_record_close(RECORD_STORAGE);
        fw_ota_set_err(err, err_size, "firmware file missing");
        return false;
    }
    const uint32_t total = (uint32_t)fi.size;

    File* f = storage_file_alloc(storage);
    esp_ota_handle_t handle = 0;
    uint8_t* chunk = NULL;
    bool ok = false;

    do {
        if(!storage_file_open(f, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            fw_ota_set_err(err, err_size, "open bin failed");
            break;
        }
        // esp_ota_begin loescht die Zielpartition (kann einige Sekunden dauern).
        esp_err_t e = esp_ota_begin(next, fi.size, &handle);
        if(e != ESP_OK) {
            fw_ota_set_err(err, err_size, "esp_ota_begin failed");
            handle = 0;
            break;
        }
        // Quellpuffer MUSS in internem DRAM liegen: esp_ota_write schreibt Flash
        // mit kurzzeitig deaktiviertem MSPI-Cache — ein PSRAM-Puffer als Quelle
        // ginge ueber den Bounce-Buffer-Pfad, der auf dieser HW heikel ist.
        chunk = heap_caps_malloc(FW_OTA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if(!chunk) {
            fw_ota_set_err(err, err_size, "out of memory");
            break;
        }

        ok = true;
        uint32_t written = 0;
        if(progress) progress(0, total, progress_ctx);
        while(written < total) {
            size_t want = (total - written) > FW_OTA_CHUNK ? FW_OTA_CHUNK : (size_t)(total - written);
            size_t r = storage_file_read(f, chunk, want);
            if(r == 0) {
                fw_ota_set_err(err, err_size, "read error");
                ok = false;
                break;
            }
            e = esp_ota_write(handle, chunk, r);
            if(e != ESP_OK) {
                fw_ota_set_err(err, err_size, "esp_ota_write failed");
                ok = false;
                break;
            }
            written += (uint32_t)r;
            if(progress) progress(written, total, progress_ctx);
        }
    } while(0);

    if(chunk) free(chunk);
    storage_file_close(f);
    storage_file_free(f);

    if(ok && handle) {
        esp_err_t e = esp_ota_end(handle); // validiert das Image (Header, Chip, SHA)
        handle = 0;
        if(e != ESP_OK) {
            fw_ota_set_err(err, err_size, "image validation failed");
            ok = false;
        } else {
            e = esp_ota_set_boot_partition(next);
            if(e != ESP_OK) {
                fw_ota_set_err(err, err_size, "set boot partition failed");
                ok = false;
            }
        }
    } else if(handle) {
        esp_ota_abort(handle);
        handle = 0;
    }

    if(ok) {
        FURI_LOG_I(TAG, "flashed %s to %s, boot set", path, next->label);
    } else {
        FURI_LOG_E(TAG, "flash %s failed: %s", path, (err && err[0]) ? err : "?");
    }

    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool fw_ota_marker_read(char* out, size_t out_size) {
    if(!out || out_size == 0) return false;
    out[0] = '\0';
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, FW_OTA_MARKER, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t r = storage_file_read(f, out, out_size - 1);
        out[r] = '\0';
        fw_ota_trim(out);
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void fw_ota_marker_write(const char* version) {
    if(!version || !version[0]) return;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, FW_OTA_MARKER, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, version, strlen(version));
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}

void fw_ota_marker_sync(void) {
    char cur[32] = {0};
    bool have = fw_ota_marker_read(cur, sizeof(cur));
    if(have && strcmp(cur, FURI_ESP32_VERSION) == 0) return;
    fw_ota_marker_write(FURI_ESP32_VERSION);
}

void fw_ota_set_resume_qflipper(bool set) {
    s_resume_qflipper = set ? FW_OTA_RESUME_QFLIPPER_MAGIC : 0;
}

bool fw_ota_take_resume_qflipper(void) {
    bool set = (s_resume_qflipper == FW_OTA_RESUME_QFLIPPER_MAGIC);
    s_resume_qflipper = 0;
    return set;
}

static void fw_ota_reboot_task(void* arg) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if(delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    FURI_LOG_I(TAG, "rebooting");
    esp_restart();
}

#if FW_OTA_HAVE_FORCE_DOWNLOAD_BOOT
static void fw_ota_download_mode_task(void* arg) {
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if(delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    FURI_LOG_I(TAG, "rebooting into ROM download mode");
    /* Das Bit wird vom ROM beim naechsten Boot ausgewertet (und geloescht):
     * der Chip bleibt im seriellen Bootloader und meldet sich per
     * USB-Serial-JTAG (303a:1001), esptool-kompatibel. */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
#endif

bool fw_ota_reboot_to_download_mode_async(uint32_t delay_ms) {
#if FW_OTA_HAVE_FORCE_DOWNLOAD_BOOT
    if(xTaskCreate(
           fw_ota_download_mode_task,
           "FwOtaDlMode",
           3072,
           (void*)(uintptr_t)delay_ms,
           5,
           NULL) != pdPASS) {
        FURI_LOG_E(TAG, "download-mode task spawn failed");
        return false;
    }
    return true;
#else
    (void)delay_ms;
    return false;
#endif
}

void fw_ota_reboot_async(uint32_t delay_ms) {
    // Eigener Task mit internem Stack: esp_restart() laeuft mit deaktiviertem
    // Cache und darf nicht auf einem PSRAM-Stack (FuriThread) ausgefuehrt werden.
    if(xTaskCreate(
           fw_ota_reboot_task, "FwOtaReboot", 3072, (void*)(uintptr_t)delay_ms, 5, NULL) !=
       pdPASS) {
        FURI_LOG_E(TAG, "reboot task spawn failed, restarting inline");
        esp_restart();
    }
}

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Gemeinsamer OTA-Kern fuer den ESP32-Port.
 *
 *  Drei Nutzer teilen sich diesen Code:
 *   - WiFi-App "Update"   (applications/main/wlan_app/wlan_fw_update.c)
 *   - RPC system_update   (components/rpc/rpc_system.c, qT-Embed/qFlipper)
 *   - OTA-Updater-App     (applications/system/ota_updater, Vollbild-Fortschritt)
 *
 *  Layout-Voraussetzung: Dual-OTA-Partitionstabelle (partitions_ota_16mb.csv,
 *  ota_0/ota_1/otadata). Auf Single-App-Layouts liefert fw_ota_is_supported()
 *  false — dann bleibt nur der USB-/Web-Flasher (esptool).
 *
 *  Threading: fw_ota_flash_file() MUSS aus einem xTaskCreate-Task mit
 *  INTERNEM DRAM-Stack laufen (esp_ota_write deaktiviert kurz den Flash-Cache;
 *  ein PSRAM-Stack fuehrt zur DoubleException). Die uebrigen Funktionen sind
 *  reines Storage-IO und aus jedem Thread aufrufbar. */

/** Staging-Pfad, den alle Update-Wege gemeinsam nutzen. */
#define FW_OTA_STAGE_DIR "/ext/update"
#define FW_OTA_STAGE_BIN FW_OTA_STAGE_DIR "/furi_esp32.bin"
/** FW-Marker: laufende Version auf der SD (getrennt von /ext/version.txt = SD-Sync). */
#define FW_OTA_MARKER "/ext/.fw_version"
/** Projektname, den ESP-IDF in den App-Descriptor schreibt (project(furi_esp32)). */
#define FW_OTA_PROJECT_NAME "furi_esp32"

typedef enum {
    FwOtaImageOk = 0,
    FwOtaImageFileNotFound,   // Datei fehlt / leer
    FwOtaImageNotAnImage,     // kein ESP-Image-Header (Magic 0xE9)
    FwOtaImageChipMismatch,   // Image fuer einen anderen Chip gebaut
    FwOtaImageWrongProject,   // App-Descriptor: nicht "furi_esp32"
    FwOtaImageReadError,
} FwOtaImageStatus;

typedef struct {
    char version[32];      // esp_app_desc_t.version (= FURI_ESP32_VERSION via PROJECT_VER)
    char project_name[32]; // esp_app_desc_t.project_name
    char date[16];         // Build-Datum
    char time[16];         // Build-Zeit
    uint16_t chip_id;      // esp_chip_id_t aus dem Image-Header
    uint32_t size;         // Dateigroesse
} FwOtaImageInfo;

/** true, wenn eine inaktive OTA-Partition existiert (Dual-OTA-Layout). */
bool fw_ota_is_supported(void);

/** Label der laufenden App-Partition ("ota_0", "ota_1", "factory"). */
const char* fw_ota_running_partition_label(void);

/** Liest Image-Header + App-Descriptor der Datei und prueft Magic, Chip-ID und
 *  Projektname. out darf NULL sein. Reines Datei-IO. */
FwOtaImageStatus fw_ota_inspect_image(const char* path, FwOtaImageInfo* out);

/** Klartext zu FwOtaImageStatus (fuer Logs/UI). */
const char* fw_ota_image_status_str(FwOtaImageStatus status);

/** Fortschritts-Callback: bytes_done/bytes_total (wird pro Chunk gerufen). */
typedef void (*FwOtaProgressCb)(uint32_t bytes_done, uint32_t bytes_total, void* ctx);

/** Flasht die .bin synchron in die inaktive OTA-Partition und setzt sie als
 *  Boot-Partition. Bei Fehler steht der Grund in err (falls != NULL).
 *  NUR aus einem Task mit internem DRAM-Stack aufrufen (siehe oben). */
bool fw_ota_flash_file(
    const char* path,
    FwOtaProgressCb progress,
    void* progress_ctx,
    char* err,
    size_t err_size);

/** FW-Marker /ext/.fw_version lesen (getrimmt). false wenn nicht vorhanden. */
bool fw_ota_marker_read(char* out, size_t out_size);

/** FW-Marker schreiben. */
void fw_ota_marker_write(const char* version);

/** Marker auf die einkompilierte Version (FURI_ESP32_VERSION) ziehen, falls er
 *  fehlt oder abweicht. No-op wenn er schon stimmt. */
void fw_ota_marker_sync(void);

/** One-shot-Flag "nach dem Reboot die qFlipper-Bridge wieder starten".
 *  Liegt in RTC_NOINIT-RAM und ueberlebt esp_restart() (nicht Power-On).
 *  Gesetzt vom OTA-Updater, wenn das Update per USB-RPC (qT-Embed) kam, damit
 *  der Host das Geraet nach dem Neustart wiederfindet; konsumiert vom Desktop
 *  beim Boot (take = lesen + loeschen). */
void fw_ota_set_resume_qflipper(bool set);
bool fw_ota_take_resume_qflipper(void);

/** Neustart aus einem eigenen Task mit internem Stack nach delay_ms.
 *  Sicher aus jedem Kontext (auch PSRAM-gestackten FuriThreads / RPC). */
void fw_ota_reboot_async(uint32_t delay_ms);

/** Neustart in den ROM-Download-Modus (serieller Bootloader, esptool-Protokoll
 *  ueber USB-Serial-JTAG). Setzt RTC_CNTL_FORCE_DOWNLOAD_BOOT und rebootet nach
 *  delay_ms aus einem Task mit internem Stack. Damit kann ein Host (qT-Embed)
 *  Bootloader + Partitionstabelle + App komplett neu flashen — z. B. der
 *  einmalige Umstieg vom Single-App- auf das Dual-OTA-Layout.
 *  Liefert false, wenn der Chip das nicht kann (kein ESP32-S3/S2). */
bool fw_ota_reboot_to_download_mode_async(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif

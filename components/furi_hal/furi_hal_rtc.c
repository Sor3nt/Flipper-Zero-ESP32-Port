#include "furi_hal_rtc.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <time.h>

#define TAG "FuriHalRtc"

#define FURI_HAL_RTC_NVS_NAMESPACE "furi_rtc"
#define FURI_HAL_RTC_NVS_KEY_STATE "registers"
#define FURI_HAL_RTC_NVS_STATE_MAGIC 0x52544331U

/* The ESP32 port has no battery-backed RTC registers. Preserve the RTC
 * backup state needed by PIN security and desktop lock behavior in internal
 * NVS so it survives resets and power loss. */
typedef struct {
    uint32_t magic;
    uint32_t flags;
    uint32_t pin_fails;
    uint32_t pin_value;
} FuriHalRtcPersistentState;

typedef struct {
    FuriHalRtcHeapTrackMode heap_track_mode;
    FuriHalRtcBootMode boot_mode;
    FuriHalRtcLogDevice log_device;
    FuriHalRtcLogBaudRate log_baud_rate;
    uint8_t log_level;
    uint32_t flags;
    uint32_t fault_data;
    uint32_t pin_fails;
    uint32_t pin_value;
    int64_t time_offset;
    FuriHalRtcLocaleTimeFormat locale_timeformat;
    FuriHalRtcLocaleDateFormat locale_dateformat;
    FuriHalRtcLocaleUnits locale_units;
} FuriHalRtcState;

static FuriHalRtcState furi_hal_rtc = {
    .heap_track_mode = FuriHalRtcHeapTrackModeNone,
    .boot_mode = FuriHalRtcBootModeNormal,
    .log_device = FuriHalRtcLogDeviceNone,
    .log_baud_rate = FuriHalRtcLogBaudRate115200,
    .log_level = 0,
    .flags = 0,
    .fault_data = 0,
    .pin_fails = 0,
    .pin_value = 0,
    .time_offset = 0,
    .locale_timeformat = FuriHalRtcLocaleTimeFormat24h,
    .locale_dateformat = FuriHalRtcLocaleDateFormatDMY,
    .locale_units = FuriHalRtcLocaleUnitsMetric,
};

static esp_err_t furi_hal_rtc_persistent_state_open(nvs_handle_t* nvs, nvs_open_mode_t mode) {
    const esp_err_t err = nvs_open(FURI_HAL_RTC_NVS_NAMESPACE, mode, nvs);
    if(err != ESP_OK && !(mode == NVS_READONLY && err == ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGE(TAG, "Unable to open persistent RTC state: %s", esp_err_to_name(err));
    }
    return err;
}

static bool furi_hal_rtc_persistent_state_load(void) {
    nvs_handle_t nvs;
    esp_err_t err = furi_hal_rtc_persistent_state_open(&nvs, NVS_READONLY);
    if(err == ESP_ERR_NVS_NOT_FOUND) return true;
    if(err != ESP_OK) return false;

    FuriHalRtcPersistentState state = {0};
    size_t state_size = sizeof(state);
    err = nvs_get_blob(nvs, FURI_HAL_RTC_NVS_KEY_STATE, &state, &state_size);
    nvs_close(nvs);

    if(err == ESP_ERR_NVS_NOT_FOUND) return true;
    if(err != ESP_OK || state_size != sizeof(state) ||
       state.magic != FURI_HAL_RTC_NVS_STATE_MAGIC) {
        ESP_LOGE(TAG, "RTC state in NVS is unreadable or has an invalid format");
        return false;
    }

    furi_hal_rtc.flags = state.flags;
    furi_hal_rtc.pin_fails = state.pin_fails;
    furi_hal_rtc.pin_value = state.pin_value;
    return true;
}

/* Argument für den NVS-Write-Helper-Task. Lebt auf dem Stack des Aufrufers
 * (ggf. PSRAM); wird nur mit aktivem Flash-Cache gelesen/geschrieben. */
typedef struct {
    FuriHalRtcPersistentState state;
    volatile bool result;
    volatile bool done;
} FuriHalRtcNvsWriteCtx;

/* Führt den eigentlichen NVS-Write aus — MUSS auf einem Task mit INTERNEM DRAM-
 * Stack laufen. nvs_commit schaltet den Flash-Cache ab; der PSRAM-Stack der
 * Aufrufer (desktop_srv-Service-Task, Settings-App) wäre dann nicht mehr
 * erreichbar → DoubleException/TG1WDT. Mit Default lock_on_boot=true + gesetztem
 * PIN passiert das schon im Boot-Pfad → Boot-Loop. Die zu schreibende Struct
 * wird auf den internen Stack dieses Tasks kopiert, damit nvs_set_blob während
 * des Cache-disable nur internes RAM berührt. Muster 1:1 aus
 * components/toolbox/saved_struct.c (project_psram_stack_nvs_doubleexception). */
static void furi_hal_rtc_nvs_write_task(void* p) {
    FuriHalRtcNvsWriteCtx* ctx = p;

    /* Lokale Kopie auf dem internen DRAM-Stack dieses Tasks (statt eines
     * PSRAM-Zeigers auf ctx->state), damit nvs_set_blob den Blob auch während
     * des Flash-Writes aus internem RAM lesen kann. */
    const FuriHalRtcPersistentState state = ctx->state;

    bool result = false;
    nvs_handle_t nvs;
    if(furi_hal_rtc_persistent_state_open(&nvs, NVS_READWRITE) == ESP_OK) {
        esp_err_t err = nvs_set_blob(nvs, FURI_HAL_RTC_NVS_KEY_STATE, &state, sizeof(state));
        if(err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);

        if(err == ESP_OK) {
            result = true;
        } else {
            ESP_LOGE(TAG, "Unable to persist RTC state in NVS: %s", esp_err_to_name(err));
        }
    }

    ctx->result = result;
    ctx->done = true;
    vTaskDelete(NULL);
}

static bool furi_hal_rtc_persistent_state_save(void) {
    /* Den NVS-Write auf einen Task mit internem DRAM-Stack auslagern — plain
     * xTaskCreate liefert einen internen Stack (kein SPIRAM_ALLOW_STACK), anders
     * als die PSRAM-gestackten Aufrufer-Tasks, die den nvs_commit sonst mit
     * DoubleException killen würden. Siehe furi_hal_rtc_nvs_write_task. */
    FuriHalRtcNvsWriteCtx ctx = {
        .state =
            {
                .magic = FURI_HAL_RTC_NVS_STATE_MAGIC,
                .flags = furi_hal_rtc.flags,
                .pin_fails = furi_hal_rtc.pin_fails,
                .pin_value = furi_hal_rtc.pin_value,
            },
        .result = false,
        .done = false,
    };

    if(xTaskCreate(furi_hal_rtc_nvs_write_task, "rtc_nvs_wr", 3072, &ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn RTC NVS write task");
        return false;
    }

    while(!ctx.done) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    return ctx.result;
}

static time_t furi_hal_rtc_now(void) {
    return time(NULL) + (time_t)furi_hal_rtc.time_offset;
}

void furi_hal_rtc_init_early(void) {
}

void furi_hal_rtc_deinit_early(void) {
}

void furi_hal_rtc_init(void) {
    furi_hal_rtc_persistent_state_load();
}

void furi_hal_rtc_prepare_for_shutdown(void) {
}

void furi_hal_rtc_sync_shadow(void) {
}

void furi_hal_rtc_reset_registers(void) {
    furi_hal_rtc.flags = 0;
    furi_hal_rtc.fault_data = 0;
    furi_hal_rtc.pin_fails = 0;
    furi_hal_rtc.pin_value = 0;
    furi_hal_rtc.boot_mode = FuriHalRtcBootModeNormal;
    furi_hal_rtc_persistent_state_save();
}

void furi_hal_rtc_set_log_level(uint8_t level) {
    furi_hal_rtc.log_level = level;
}

uint8_t furi_hal_rtc_get_log_level(void) {
    return furi_hal_rtc.log_level;
}

void furi_hal_rtc_set_log_device(FuriHalRtcLogDevice device) {
    furi_hal_rtc.log_device = device;
}

FuriHalRtcLogDevice furi_hal_rtc_get_log_device(void) {
    return furi_hal_rtc.log_device;
}

void furi_hal_rtc_set_log_baud_rate(FuriHalRtcLogBaudRate baud_rate) {
    furi_hal_rtc.log_baud_rate = baud_rate;
}

FuriHalRtcLogBaudRate furi_hal_rtc_get_log_baud_rate(void) {
    return furi_hal_rtc.log_baud_rate;
}

void furi_hal_rtc_set_fault_data(uint32_t value) {
    furi_hal_rtc.fault_data = value;
}

uint32_t furi_hal_rtc_get_fault_data(void) {
    return furi_hal_rtc.fault_data;
}

void furi_hal_rtc_set_pin_fails(uint32_t value) {
    if(furi_hal_rtc.pin_fails == value) return;
    furi_hal_rtc.pin_fails = value;
    furi_hal_rtc_persistent_state_save();
}

uint32_t furi_hal_rtc_get_pin_fails(void) {
    return furi_hal_rtc.pin_fails;
}

bool furi_hal_rtc_set_pin_value(uint32_t value) {
    if(furi_hal_rtc.pin_value == value) return true;

    const uint32_t previous = furi_hal_rtc.pin_value;
    furi_hal_rtc.pin_value = value;
    if(!furi_hal_rtc_persistent_state_save()) {
        furi_hal_rtc.pin_value = previous;
        return false;
    }

    return true;
}

uint32_t furi_hal_rtc_get_pin_value(void) {
    return furi_hal_rtc.pin_value;
}

bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return (furi_hal_rtc.flags & flag) != 0;
}

void furi_hal_rtc_set_flag(FuriHalRtcFlag flag) {
    const uint32_t previous = furi_hal_rtc.flags;
    furi_hal_rtc.flags |= flag;
    if(furi_hal_rtc.flags != previous) furi_hal_rtc_persistent_state_save();
}

void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag) {
    const uint32_t previous = furi_hal_rtc.flags;
    furi_hal_rtc.flags &= ~flag;
    if(furi_hal_rtc.flags != previous) furi_hal_rtc_persistent_state_save();
}

FuriHalRtcBootMode furi_hal_rtc_get_boot_mode(void) {
    return furi_hal_rtc.boot_mode;
}

void furi_hal_rtc_set_boot_mode(FuriHalRtcBootMode mode) {
    furi_hal_rtc.boot_mode = mode;
}

FuriHalRtcHeapTrackMode furi_hal_rtc_get_heap_track_mode(void) {
    return furi_hal_rtc.heap_track_mode;
}

void furi_hal_rtc_set_heap_track_mode(FuriHalRtcHeapTrackMode mode) {
    furi_hal_rtc.heap_track_mode = mode;
}

void furi_hal_rtc_get_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    time_t now = furi_hal_rtc_now();
    struct tm now_tm = {0};
    localtime_r(&now, &now_tm);

    datetime->hour = now_tm.tm_hour;
    datetime->minute = now_tm.tm_min;
    datetime->second = now_tm.tm_sec;
    datetime->day = now_tm.tm_mday;
    datetime->month = now_tm.tm_mon + 1;
    datetime->year = now_tm.tm_year + 1900;
    datetime->weekday = ((now_tm.tm_wday + 6) % 7) + 1;
}

void furi_hal_rtc_set_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    struct tm desired = {
        .tm_sec = datetime->second,
        .tm_min = datetime->minute,
        .tm_hour = datetime->hour,
        .tm_mday = datetime->day,
        .tm_mon = datetime->month - 1,
        .tm_year = datetime->year - 1900,
        .tm_isdst = -1,
    };

    const time_t target = mktime(&desired);
    if(target != (time_t)-1) {
        furi_hal_rtc.time_offset = (int64_t)target - (int64_t)time(NULL);
    }
}

uint32_t furi_hal_rtc_get_timestamp(void) {
    return (uint32_t)furi_hal_rtc_now();
}

FuriHalRtcLocaleTimeFormat furi_hal_rtc_get_locale_timeformat(void) {
    return furi_hal_rtc.locale_timeformat;
}

void furi_hal_rtc_set_locale_timeformat(FuriHalRtcLocaleTimeFormat format) {
    furi_hal_rtc.locale_timeformat = format;
}

FuriHalRtcLocaleDateFormat furi_hal_rtc_get_locale_dateformat(void) {
    return furi_hal_rtc.locale_dateformat;
}

void furi_hal_rtc_set_locale_dateformat(FuriHalRtcLocaleDateFormat format) {
    furi_hal_rtc.locale_dateformat = format;
}

FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void) {
    return furi_hal_rtc.locale_units;
}

void furi_hal_rtc_set_locale_units(FuriHalRtcLocaleUnits format) {
    furi_hal_rtc.locale_units = format;
}

#include "furi_hal_rtc.h"

#include <sys/time.h>
#include <time.h>

#include <furi.h>
#include <nvs.h>

#define TAG "FuriHalRtc"

/* On the Flipper the PIN, the failed-attempt counter and the Lock flag live in
 * RTC backup registers and survive a reboot. Here they are kept in NVS so
 * "Lock on Boot if PIN set" and the PIN fail timeout cannot be skipped by
 * rebooting. Written only when a value changes. */
#define FURI_HAL_RTC_NVS_NAMESPACE "furi_rtc"
#define FURI_HAL_RTC_NVS_PIN       "pin"
#define FURI_HAL_RTC_NVS_PIN_FAILS "pin_fails"
#define FURI_HAL_RTC_NVS_LOCK      "lock"

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
    .locale_timeformat = FuriHalRtcLocaleTimeFormat24h,
    .locale_dateformat = FuriHalRtcLocaleDateFormatDMY,
    .locale_units = FuriHalRtcLocaleUnitsMetric,
};

/* The wall clock is the system clock itself. It used to be "system clock + a RAM
 * offset": a time set by the user or qFlipper was lost on every reboot, and once
 * SNTP (wlan_hal) stepped the system clock to the real time the offset was added a
 * second time, so the date came out roughly twice too far in the future. The system
 * clock is backed by the RTC timer (CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER), so it also
 * survives a soft reset and deep sleep. */
static time_t furi_hal_rtc_now(void) {
    return time(NULL);
}

void furi_hal_rtc_init_early(void) {
}

void furi_hal_rtc_deinit_early(void) {
}

static void furi_hal_rtc_nvs_write_u32(const char* key, uint32_t value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FURI_HAL_RTC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(err == ESP_OK) {
        err = nvs_set_u32(handle, key, value);
        if(err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "NVS write '%s' failed: %s", key, esp_err_to_name(err));
    }
}

void furi_hal_rtc_init(void) {
    /* Runs after nvs_flash_init() in furi_hal_init(). */
    nvs_handle_t handle;
    if(nvs_open(FURI_HAL_RTC_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;

    uint32_t value;
    if(nvs_get_u32(handle, FURI_HAL_RTC_NVS_PIN, &value) == ESP_OK) {
        furi_hal_rtc.pin_value = value;
    }
    if(nvs_get_u32(handle, FURI_HAL_RTC_NVS_PIN_FAILS, &value) == ESP_OK) {
        furi_hal_rtc.pin_fails = value;
    }
    if(nvs_get_u32(handle, FURI_HAL_RTC_NVS_LOCK, &value) == ESP_OK && value) {
        furi_hal_rtc.flags |= FuriHalRtcFlagLock;
    }
    nvs_close(handle);
}

void furi_hal_rtc_prepare_for_shutdown(void) {
}

void furi_hal_rtc_sync_shadow(void) {
}

void furi_hal_rtc_reset_registers(void) {
    const bool was_locked = furi_hal_rtc.flags & FuriHalRtcFlagLock;
    furi_hal_rtc.flags = 0;
    furi_hal_rtc.fault_data = 0;
    furi_hal_rtc_set_pin_fails(0);
    furi_hal_rtc_set_pin_value(0);
    furi_hal_rtc.boot_mode = FuriHalRtcBootModeNormal;
    if(was_locked) furi_hal_rtc_nvs_write_u32(FURI_HAL_RTC_NVS_LOCK, 0);
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
    furi_hal_rtc_nvs_write_u32(FURI_HAL_RTC_NVS_PIN_FAILS, value);
}

uint32_t furi_hal_rtc_get_pin_fails(void) {
    return furi_hal_rtc.pin_fails;
}

void furi_hal_rtc_set_pin_value(uint32_t value) {
    if(furi_hal_rtc.pin_value == value) return;
    furi_hal_rtc.pin_value = value;
    furi_hal_rtc_nvs_write_u32(FURI_HAL_RTC_NVS_PIN, value);
}

uint32_t furi_hal_rtc_get_pin_value(void) {
    return furi_hal_rtc.pin_value;
}

/* Services set and clear different flags from different threads: do the
 * read-modify-write atomically so one update cannot overwrite another. */
bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return (__atomic_load_n(&furi_hal_rtc.flags, __ATOMIC_ACQUIRE) & (uint32_t)flag) != 0;
}

void furi_hal_rtc_set_flag(FuriHalRtcFlag flag) {
    uint32_t prev = __atomic_fetch_or(&furi_hal_rtc.flags, (uint32_t)flag, __ATOMIC_ACQ_REL);
    if((flag & FuriHalRtcFlagLock) && !(prev & FuriHalRtcFlagLock)) {
        furi_hal_rtc_nvs_write_u32(FURI_HAL_RTC_NVS_LOCK, 1);
    }
}

void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag) {
    uint32_t prev = __atomic_fetch_and(&furi_hal_rtc.flags, ~(uint32_t)flag, __ATOMIC_ACQ_REL);
    if((flag & FuriHalRtcFlagLock) && (prev & FuriHalRtcFlagLock)) {
        furi_hal_rtc_nvs_write_u32(FURI_HAL_RTC_NVS_LOCK, 0);
    }
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
    if(target == (time_t)-1) {
        FURI_LOG_W(TAG, "Ignoring an invalid date/time");
        return;
    }

    const struct timeval tv = {.tv_sec = target, .tv_usec = 0};
    if(settimeofday(&tv, NULL) != 0) {
        FURI_LOG_E(TAG, "Could not set the system clock");
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

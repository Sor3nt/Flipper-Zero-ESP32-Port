#include "furi_hal_rtc.h"

#include <nvs.h>
#include <time.h>

#define FURI_RTC_NVS_NAMESPACE "furi"
#define FURI_RTC_NVS_KEY_OFFSET "rtc_off"

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

static DateTime furi_hal_rtc_alarm_time;
static bool furi_hal_rtc_alarm_enabled;

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

static void furi_hal_rtc_load_offset_from_nvs(void) {
    nvs_handle_t handle;
    if(nvs_open(FURI_RTC_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    int64_t stored = 0;
    esp_err_t err = nvs_get_i64(handle, FURI_RTC_NVS_KEY_OFFSET, &stored);
    nvs_close(handle);
    if(err == ESP_OK) {
        furi_hal_rtc.time_offset = stored;
    }
}

static void furi_hal_rtc_save_offset_to_nvs(void) {
    nvs_handle_t handle;
    if(nvs_open(FURI_RTC_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_i64(handle, FURI_RTC_NVS_KEY_OFFSET, furi_hal_rtc.time_offset);
    nvs_commit(handle);
    nvs_close(handle);
}

static time_t furi_hal_rtc_now(void) {
    return time(NULL) + (time_t)furi_hal_rtc.time_offset;
}

void furi_hal_rtc_init_early(void) {
}

void furi_hal_rtc_deinit_early(void) {
}

void furi_hal_rtc_init(void) {
    furi_hal_rtc_load_offset_from_nvs();
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
    furi_hal_rtc.pin_fails = value;
}

uint32_t furi_hal_rtc_get_pin_fails(void) {
    return furi_hal_rtc.pin_fails;
}

void furi_hal_rtc_set_pin_value(uint32_t value) {
    furi_hal_rtc.pin_value = value;
}

uint32_t furi_hal_rtc_get_pin_value(void) {
    return furi_hal_rtc.pin_value;
}

bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return (furi_hal_rtc.flags & flag) != 0;
}

void furi_hal_rtc_set_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags |= flag;
}

void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags &= ~flag;
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
        furi_hal_rtc_save_offset_to_nvs();
    }
}

uint32_t furi_hal_rtc_get_timestamp(void) {
    return (uint32_t)furi_hal_rtc_now();
}

void furi_hal_rtc_set_alarm(const DateTime* datetime, bool enabled) {
    if(enabled && datetime) {
        furi_hal_rtc_alarm_time = *datetime;
        furi_hal_rtc_alarm_enabled = true;
    } else {
        furi_hal_rtc_alarm_enabled = false;
    }
}

bool furi_hal_rtc_get_alarm(DateTime* datetime) {
    if(datetime) {
        *datetime = furi_hal_rtc_alarm_time;
    }
    return furi_hal_rtc_alarm_enabled;
}

void furi_hal_rtc_set_alarm_callback(FuriHalRtcAlarmCallback callback, void* context) {
    (void)callback;
    (void)context;
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

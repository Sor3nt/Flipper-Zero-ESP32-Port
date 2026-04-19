#pragma once

#include <stdbool.h>
#include <furi.h>
#include <furi_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LocaleMeasurementUnitsMetric = 0,
    LocaleMeasurementUnitsImperial = 1,
} LocaleMeasurementUnits;

typedef enum {
    LocaleTimeFormat24h = 0,
    LocaleTimeFormat12h = 1,
} LocaleTimeFormat;

typedef enum {
    LocaleDateFormatDMY = 0,
    LocaleDateFormatMDY = 1,
    LocaleDateFormatYMD = 2,
} LocaleDateFormat;

LocaleMeasurementUnits locale_get_measurement_unit(void);
void locale_set_measurement_unit(LocaleMeasurementUnits format);
float locale_fahrenheit_to_celsius(float temp_f);
float locale_celsius_to_fahrenheit(float temp_c);
LocaleTimeFormat locale_get_time_format(void);
void locale_set_time_format(LocaleTimeFormat format);
void locale_format_time(
    FuriString* out_str,
    const DateTime* datetime,
    const LocaleTimeFormat format,
    const bool show_seconds);
LocaleDateFormat locale_get_date_format(void);
void locale_set_date_format(LocaleDateFormat format);
void locale_format_date(
    FuriString* out_str,
    const DateTime* datetime,
    const LocaleDateFormat format,
    const char* separator);
void locale_on_system_start(void);

#ifdef __cplusplus
}
#endif

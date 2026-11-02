/**
 * @file furi_hal_bq27220.h
 * BQ27220 fuel gauge driver (I2C)
 * API mirrors STM32 Flipper firmware (lib/drivers/bq27220.h)
 *
 * All readings come from one cached snapshot that is refreshed at most once
 * per FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS. TI SLUUBD4A section 5.3 warns that
 * reading the standard commands more than twice per second can reset the gauge
 * through its watchdog, and every getter used to issue its own I2C read.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BatteryStatus() (0x0A) bits, TI SLUUBD4A table 2-6. */
#define FURI_HAL_BQ27220_STATUS_DSG      (1U << 0) /**< Discharging (clear = charging/relax) */
#define FURI_HAL_BQ27220_STATUS_SYSDWN   (1U << 1) /**< System should shut down */
#define FURI_HAL_BQ27220_STATUS_TDA      (1U << 2) /**< Terminate discharge alarm */
#define FURI_HAL_BQ27220_STATUS_BATTPRES (1U << 3) /**< Battery present */
#define FURI_HAL_BQ27220_STATUS_TCA      (1U << 6) /**< Terminate charge alarm */
#define FURI_HAL_BQ27220_STATUS_CHGINH   (1U << 8) /**< Charge inhibited by temperature */
#define FURI_HAL_BQ27220_STATUS_FC       (1U << 9) /**< Full charge detected */
#define FURI_HAL_BQ27220_STATUS_OTD      (1U << 10) /**< Over-temperature in discharge */
#define FURI_HAL_BQ27220_STATUS_OTC      (1U << 11) /**< Over-temperature in charge */
#define FURI_HAL_BQ27220_STATUS_SLEEP    (1U << 12) /**< Gauge is in SLEEP mode */
#define FURI_HAL_BQ27220_STATUS_FD       (1U << 15) /**< Full discharge detected */

/** OperationStatus() (0x3A) bits. */
#define FURI_HAL_BQ27220_OPSTATUS_INITCOMP  (1U << 5) /**< Gauge initialization complete */
#define FURI_HAL_BQ27220_OPSTATUS_CFGUPDATE (1U << 10) /**< CONFIG UPDATE mode active */

/** TimeToEmpty()/TimeToFull() value meaning "not (dis)charging". */
#define FURI_HAL_BQ27220_TIME_NONE (0xFFFFU)

typedef struct {
    bool valid; /**< At least one complete snapshot has been read */
    bool fresh; /**< Most recent refresh attempt succeeded; use tick for data age */
    bool battery_present; /**< BAT voltage high enough for a cell to be fitted */
    uint32_t tick; /**< FreeRTOS tick of the last successful refresh */
    uint16_t temperature_dk; /**< 0.1 K */
    uint16_t voltage_mv;
    uint16_t battery_status; /**< FURI_HAL_BQ27220_STATUS_* */
    int16_t current_ma; /**< Instantaneous, negative = discharging */
    uint16_t remaining_capacity_mah;
    uint16_t full_charge_capacity_mah;
    int16_t average_current_ma;
    uint16_t time_to_empty_min; /**< FURI_HAL_BQ27220_TIME_NONE when not discharging */
    uint16_t time_to_full_min; /**< FURI_HAL_BQ27220_TIME_NONE when not charging */
    uint16_t cycle_count;
    uint16_t state_of_charge; /**< % */
    uint16_t state_of_health; /**< % */
    uint16_t operation_status; /**< FURI_HAL_BQ27220_OPSTATUS_* */
} FuriHalBq27220Snapshot;

/** Initialize BQ27220 on the Qwiic/Grove I2C bus.
 *  @return true if the device was found and responded */
bool furi_hal_bq27220_init(void);

/** Check if the BQ27220 has been detected since boot. This remains true through
 * temporary read failures; use furi_hal_bq27220_is_data_valid() for recent data.
 * If it was not found at boot, the bus is re-probed at most every 10 s. */
bool furi_hal_bq27220_is_present(void);

/** True when the snapshot holds battery data: a cell is fitted and a complete
 * snapshot succeeded within the last 10 s. */
bool furi_hal_bq27220_is_data_valid(void);

/** Copy the cached snapshot, refreshing it first if it is older than
 * FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS. On I2C errors the previous values are
 * kept (fresh=false), so a transient bus error never shows up as 0%. Check
 * tick/is_data_valid() before treating preserved values as current.
 * @return true if out->valid */
bool furi_hal_bq27220_get_snapshot(FuriHalBq27220Snapshot* out);

/** Get battery charge percentage (0–100) — StateOfCharge register */
uint8_t furi_hal_bq27220_get_charge_pct(void);

/** Get state of health percentage (0–100) */
uint8_t furi_hal_bq27220_get_health_pct(void);

/** Get whether the battery is currently charging (DSG bit = 0) */
bool furi_hal_bq27220_is_charging(void);

/** Full charge detected (BatteryStatus FC) */
bool furi_hal_bq27220_is_full_charged(void);

/** Gauge requests system shutdown (BatteryStatus SYSDWN, Voltage() below SysDown Set Volt Threshold) */
bool furi_hal_bq27220_is_sysdown(void);

/** Get battery voltage in mV */
uint16_t furi_hal_bq27220_get_voltage_mv(void);

/** Get instantaneous current in mA (signed: negative = discharging) */
int16_t furi_hal_bq27220_get_current_ma(void);

/** Get filtered current in mA (signed: negative = discharging) */
int16_t furi_hal_bq27220_get_average_current_ma(void);

/** Get battery temperature in 0.1°K units (convert: (raw - 2731) / 10.0 = °C) */
uint16_t furi_hal_bq27220_get_temperature_raw(void);

/** Get remaining capacity in mAh */
uint16_t furi_hal_bq27220_get_remaining_capacity_mah(void);

/** Get full charge capacity in mAh */
uint16_t furi_hal_bq27220_get_full_charge_capacity_mah(void);

/** Get design capacity in mAh (read once at init) */
uint16_t furi_hal_bq27220_get_design_capacity_mah(void);

/** Minutes to empty/full, FURI_HAL_BQ27220_TIME_NONE when not applicable */
uint16_t furi_hal_bq27220_get_time_to_empty_min(void);
uint16_t furi_hal_bq27220_get_time_to_full_min(void);

/** Charge cycles counted by the gauge */
uint16_t furi_hal_bq27220_get_cycle_count(void);

/** Device number (0x0220 for BQ27220) and firmware version read at init, 0 if unknown */
uint16_t furi_hal_bq27220_get_device_number(void);
uint16_t furi_hal_bq27220_get_fw_version(void);

#ifdef __cplusplus
}
#endif

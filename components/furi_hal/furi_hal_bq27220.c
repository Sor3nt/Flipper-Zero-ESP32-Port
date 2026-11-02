/**
 * @file furi_hal_bq27220.c
 * BQ27220 fuel gauge driver — ESP-IDF I2C (legacy driver API)
 * API mirrors STM32 Flipper firmware (lib/drivers/bq27220.c).
 *
 * I2C address: 0x55
 * Bus: Qwiic/Grove (shared with BQ25896 and the PN532)
 *
 * Notes from TI SLUUBD4A (BQ27220 TRM), chapter 5:
 *  - Do not read the standard commands more than twice per second, or the
 *    gauge watchdog may reset it. Everything is therefore read into one cached
 *    snapshot, refreshed at most once per FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS.
 *  - Keep t(BUF) >= 66 us bus-free time between packets.
 *  - The gauge may stretch SCL for up to 4 ms (100 us in SLEEP mode).
 *  - I2C traffic does not take the gauge out of its own SLEEP mode; only a
 *    current above Sleep Current does.
 *
 * The gauge is powered from the BAT pin through its own LDO (VBAT >= 2.45 V,
 * BQ27220 data sheet). Without a cell, BAT only carries what the charger puts
 * there while it probes for a battery, so the gauge keeps browning out: it may
 * not ACK at boot, and readings come and go. Detection is therefore retried at
 * boot and re-probed later, and "no battery" is reported as such instead of as
 * read errors.
 *
 * Data memory is NOT written here. The CEDV profile (capacity, EDV points,
 * discharge curve) is cell specific; writing a wrong profile makes SoC worse
 * than the ROM defaults. The design capacity is compared with the board value
 * at init and a mismatch is logged so it can be configured deliberately.
 */

#include "furi_hal_bq27220.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "BQ27220";

/* Standard commands (TI SLUUBD4A table 2-1) */
#define BQ27220_CMD_CONTROL          0x00
#define BQ27220_CMD_TEMPERATURE      0x06
#define BQ27220_CMD_REMAINING_CAP    0x10
#define BQ27220_CMD_CYCLE_COUNT      0x2A
#define BQ27220_CMD_OPERATION_STATUS 0x3A
#define BQ27220_CMD_DESIGN_CAP       0x3C
#define BQ27220_CMD_MAC_DATA         0x40

/* Control() subcommands */
#define BQ27220_CTRL_DEVICE_NUMBER 0x0001
#define BQ27220_CTRL_FW_VERSION    0x0002

#define BQ27220_DEVICE_ID 0x0220

#ifndef BQ_I2C_PORT
#define BQ_I2C_PORT I2C_NUM_0
#endif

#ifndef BQ27220_ADDR
#define BQ27220_ADDR 0x55
#endif

/* Covers the 4 ms worst-case clock stretch with margin. */
#define BQ27220_I2C_TIMEOUT pdMS_TO_TICKS(100)
#define BQ27220_I2C_RETRIES 2
/* t(BUF) is 66 us; a little extra costs nothing at one snapshot per second. */
#define BQ27220_BUS_FREE_US 100
/* Upstream Flipper driver: MAC reads fail below ~500 us after a subcommand. */
#define BQ27220_SUBCMD_DELAY_US 1000

#ifndef FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS
#define FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS 1000U
#endif

/* Boot probe: covers the 13 ms initial VDD delay of the gauge LDO. */
#define BQ27220_PROBE_ATTEMPTS 5
#define BQ27220_PROBE_RETRY_MS 20
/* Not found at boot: probe again this often (battery inserted later). */
#define BQ27220_REPROBE_MS 10000U
/* Below this the gauge LDO cannot run from the cell: no battery on BAT. */
#define BQ27220_BATTERY_MIN_MV 2500U
/* Data older than this (refreshes failing) no longer counts as valid. */
#define BQ27220_DATA_TIMEOUT_MS 10000U
/* Without a cell the gauge flaps; log its state changes at most this often. */
#define BQ27220_LOG_INTERVAL_MS 60000U

static bool bq27220_initialized = false;
static bool bq27220_present = false;
static uint16_t bq27220_design_capacity_mah = 0;
static uint16_t bq27220_device_number = 0;
static uint16_t bq27220_fw_version = 0;
static SemaphoreHandle_t bq27220_mutex = NULL;
static portMUX_TYPE bq27220_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static FuriHalBq27220Snapshot bq27220_snapshot = {0};
static bool bq27220_snapshot_attempted = false;
static TickType_t bq27220_snapshot_attempt_tick = 0;
static uint32_t bq27220_error_count = 0;
static TickType_t bq27220_last_probe_tick = 0;
static bool bq27220_link_ok = true;
static bool bq27220_battery_reported = true;
static TickType_t bq27220_log_tick = 0;
static bool bq27220_log_started = false;

static FuriHalBq27220Snapshot bq27220_snapshot_copy(void) {
    FuriHalBq27220Snapshot snapshot;
    portENTER_CRITICAL(&bq27220_snapshot_lock);
    snapshot = bq27220_snapshot;
    portEXIT_CRITICAL(&bq27220_snapshot_lock);
    return snapshot;
}

static bool bq27220_log_allowed(void) {
    const TickType_t now = xTaskGetTickCount();
    if(bq27220_log_started &&
       (TickType_t)(now - bq27220_log_tick) < pdMS_TO_TICKS(BQ27220_LOG_INTERVAL_MS)) {
        return false;
    }
    bq27220_log_started = true;
    bq27220_log_tick = now;
    return true;
}

static bool bq27220_read(uint8_t reg, uint8_t* data, size_t len) {
    for(int attempt = 0; attempt < BQ27220_I2C_RETRIES; attempt++) {
        const esp_err_t err = i2c_master_write_read_device(
            BQ_I2C_PORT, BQ27220_ADDR, &reg, 1, data, len, BQ27220_I2C_TIMEOUT);
        esp_rom_delay_us(BQ27220_BUS_FREE_US);
        if(err == ESP_OK) return true;
    }
    return false;
}

static bool bq27220_read_word(uint8_t reg, uint16_t* value) {
    uint8_t data[2] = {0};
    if(!bq27220_read(reg, data, sizeof(data))) return false;
    *value = (uint16_t)((uint16_t)data[1] << 8 | data[0]);
    return true;
}

/* Control() subcommand whose result is returned in MACData (upstream bq27220.c). */
static bool bq27220_read_subcommand(uint16_t subcommand, uint16_t* value) {
    const uint8_t cmd[3] = {
        BQ27220_CMD_CONTROL, (uint8_t)(subcommand & 0xFF), (uint8_t)(subcommand >> 8)};
    if(i2c_master_write_to_device(BQ_I2C_PORT, BQ27220_ADDR, cmd, sizeof(cmd), BQ27220_I2C_TIMEOUT) !=
       ESP_OK) {
        return false;
    }
    esp_rom_delay_us(BQ27220_SUBCMD_DELAY_US);
    return bq27220_read_word(BQ27220_CMD_MAC_DATA, value);
}

static inline uint16_t bq27220_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

/* Four incremental reads cover every command the firmware uses:
 *   0x06..0x0D Temperature, Voltage, BatteryStatus, Current
 *   0x10..0x19 RemainingCapacity, FullChargeCapacity, AverageCurrent, TimeToEmpty, TimeToFull
 *   0x2A..0x2F CycleCount, StateOfCharge, StateOfHealth
 *   0x3A       OperationStatus
 * Caller holds bq27220_mutex. */
static bool bq27220_refresh_locked(void) {
    uint8_t a[8], b[10], c[6], d[2];
    if(!bq27220_read(BQ27220_CMD_TEMPERATURE, a, sizeof(a)) ||
       !bq27220_read(BQ27220_CMD_REMAINING_CAP, b, sizeof(b)) ||
       !bq27220_read(BQ27220_CMD_CYCLE_COUNT, c, sizeof(c)) ||
       !bq27220_read(BQ27220_CMD_OPERATION_STATUS, d, sizeof(d))) {
        return false;
    }

    const uint16_t voltage_mv = bq27220_le16(&a[2]);
    const uint16_t soc = bq27220_le16(&c[2]);
    /* Impossible values are a bus glitch, not data. */
    if(voltage_mv > 6000U || soc > 100U) {
        return false;
    }

    FuriHalBq27220Snapshot snapshot = {0};
    /* A low BAT voltage is a missing cell, a valid answer rather than an
     * error. Consumers must not read it as an empty battery. */
    snapshot.battery_present = voltage_mv >= BQ27220_BATTERY_MIN_MV;
    snapshot.temperature_dk = bq27220_le16(&a[0]);
    snapshot.voltage_mv = voltage_mv;
    snapshot.battery_status = bq27220_le16(&a[4]);
    snapshot.current_ma = (int16_t)bq27220_le16(&a[6]);
    snapshot.remaining_capacity_mah = bq27220_le16(&b[0]);
    snapshot.full_charge_capacity_mah = bq27220_le16(&b[2]);
    snapshot.average_current_ma = (int16_t)bq27220_le16(&b[4]);
    snapshot.time_to_empty_min = bq27220_le16(&b[6]);
    snapshot.time_to_full_min = bq27220_le16(&b[8]);
    snapshot.cycle_count = bq27220_le16(&c[0]);
    snapshot.state_of_charge = soc;
    snapshot.state_of_health = bq27220_le16(&c[4]);
    if(snapshot.state_of_health > 100U) snapshot.state_of_health = 100U;
    snapshot.operation_status = bq27220_le16(&d[0]);
    snapshot.tick = xTaskGetTickCount();
    snapshot.valid = true;
    snapshot.fresh = true;
    portENTER_CRITICAL(&bq27220_snapshot_lock);
    bq27220_snapshot = snapshot;
    portEXIT_CRITICAL(&bq27220_snapshot_lock);
    return true;
}

static bool bq27220_snapshot_get(FuriHalBq27220Snapshot* out) {
    if(!bq27220_present || !bq27220_mutex) {
        if(out) memset(out, 0, sizeof(*out));
        return false;
    }
    if(xSemaphoreTake(bq27220_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        /* Another thread is in a slow refresh: hand out a consistent cached
         * snapshot rather than zeros, which would read as an empty battery. */
        FuriHalBq27220Snapshot last = bq27220_snapshot_copy();
        last.fresh = false;
        if(out) *out = last;
        return last.valid;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t max_age = pdMS_TO_TICKS(FURI_HAL_BQ27220_SNAPSHOT_MAX_AGE_MS);
    /* Rate limit attempts too, so a dead bus is not hammered by every getter. */
    if(!bq27220_snapshot_attempted || (TickType_t)(now - bq27220_snapshot_attempt_tick) >= max_age) {
        bq27220_snapshot_attempted = true;
        bq27220_snapshot_attempt_tick = now;
        if(bq27220_refresh_locked()) {
            if(!bq27220_link_ok) {
                bq27220_link_ok = true;
                ESP_LOGD(TAG, "Gauge responding again (failed reads so far: %lu)",
                    (unsigned long)bq27220_error_count);
            }
            if(bq27220_snapshot.battery_present != bq27220_battery_reported) {
                bq27220_battery_reported = bq27220_snapshot.battery_present;
                if(bq27220_battery_reported) {
                    ESP_LOGI(TAG, "Battery detected (%umV)", bq27220_snapshot.voltage_mv);
                } else if(bq27220_log_allowed()) {
                    ESP_LOGW(
                        TAG,
                        "No battery on BAT (%umV); battery data unavailable",
                        bq27220_snapshot.voltage_mv);
                }
            }
        } else {
            portENTER_CRITICAL(&bq27220_snapshot_lock);
            bq27220_snapshot.fresh = false;
            portEXIT_CRITICAL(&bq27220_snapshot_lock);
            bq27220_error_count++;
            if(bq27220_link_ok) {
                bq27220_link_ok = false;
                if(bq27220_log_allowed()) {
                    ESP_LOGW(
                        TAG,
                        "Gauge not responding (no battery or gauge resetting); keeping last "
                        "values (failed reads: %lu)",
                        (unsigned long)bq27220_error_count);
                }
            }
        }
    }
    const FuriHalBq27220Snapshot snapshot = bq27220_snapshot_copy();
    if(out) *out = snapshot;
    const bool valid = snapshot.valid;
    xSemaphoreGive(bq27220_mutex);
    return valid;
}

static bool bq27220_ack(void) {
    i2c_cmd_handle_t probe_cmd = i2c_cmd_link_create();
    if(!probe_cmd) {
        ESP_LOGE(TAG, "Unable to allocate I2C probe command");
        return false;
    }

    esp_err_t probe_err = i2c_master_start(probe_cmd);
    if(probe_err == ESP_OK) {
        probe_err = i2c_master_write_byte(
            probe_cmd, (BQ27220_ADDR << 1) | I2C_MASTER_WRITE, true);
    }
    if(probe_err == ESP_OK) probe_err = i2c_master_stop(probe_cmd);
    if(probe_err == ESP_OK) {
        probe_err = i2c_master_cmd_begin(BQ_I2C_PORT, probe_cmd, BQ27220_I2C_TIMEOUT);
    }
    i2c_cmd_link_delete(probe_cmd);
    esp_rom_delay_us(BQ27220_BUS_FREE_US);
    return probe_err == ESP_OK;
}

/* Gauge answered: read identity, take the first snapshot. Caller holds the mutex. */
static void bq27220_identify_locked(void) {
    if(bq27220_read_subcommand(BQ27220_CTRL_DEVICE_NUMBER, &bq27220_device_number) &&
       bq27220_device_number != BQ27220_DEVICE_ID) {
        ESP_LOGW(
            TAG,
            "Unexpected device number 0x%04X (BQ27220 = 0x%04X); continuing",
            bq27220_device_number,
            BQ27220_DEVICE_ID);
    }
    if(!bq27220_read_subcommand(BQ27220_CTRL_FW_VERSION, &bq27220_fw_version)) {
        bq27220_fw_version = 0;
    }
    if(!bq27220_read_word(BQ27220_CMD_DESIGN_CAP, &bq27220_design_capacity_mah)) {
        bq27220_design_capacity_mah = 0;
    }

    bq27220_present = true;
    bq27220_snapshot_attempted = true;
    bq27220_snapshot_attempt_tick = xTaskGetTickCount();
    if(bq27220_refresh_locked()) {
        bq27220_battery_reported = bq27220_snapshot.battery_present;
    }
    const FuriHalBq27220Snapshot s = bq27220_snapshot_copy();

    ESP_LOGI(
        TAG,
        "BQ27220 found: dev=0x%04X fw=0x%04X voltage=%umV soc=%u%% soh=%u%% design=%umAh "
        "fcc=%umAh cycles=%u status=0x%04X op=0x%04X%s",
        bq27220_device_number,
        bq27220_fw_version,
        s.voltage_mv,
        s.state_of_charge,
        s.state_of_health,
        bq27220_design_capacity_mah,
        s.full_charge_capacity_mah,
        s.cycle_count,
        s.battery_status,
        s.operation_status,
        s.valid ? "" : " (snapshot invalid)");

    if(s.valid && !s.battery_present) {
        ESP_LOGW(TAG, "No battery on BAT (%umV); battery data unavailable", s.voltage_mv);
    } else if(s.valid && !(s.operation_status & FURI_HAL_BQ27220_OPSTATUS_INITCOMP)) {
        ESP_LOGW(TAG, "Gauge initialization not complete yet; SoC may settle in a few seconds");
    }
#ifdef FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH
    if(bq27220_design_capacity_mah) {
        const uint32_t board = FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH;
        const uint32_t gauge = bq27220_design_capacity_mah;
        const uint32_t diff = gauge > board ? gauge - board : board - gauge;
        if(diff * 10U > board) {
            ESP_LOGW(
                TAG,
                "Gauge design capacity %lumAh differs from board battery %lumAh: the gauge "
                "profile is not configured for this cell, SoC/capacity will be approximate",
                (unsigned long)gauge,
                (unsigned long)board);
        }
    }
#endif
}

bool furi_hal_bq27220_init(void) {
    if(bq27220_initialized) return bq27220_present;

    /* I2C bus assumed already initialized by furi_hal_power_init() */
    if(!bq27220_mutex) {
        bq27220_mutex = xSemaphoreCreateMutex();
        if(!bq27220_mutex) {
            ESP_LOGE(TAG, "Mutex allocation failed; gauge disabled");
            return false;
        }
    }
    bq27220_initialized = true;

    xSemaphoreTake(bq27220_mutex, portMAX_DELAY);
    for(int attempt = 0; attempt < BQ27220_PROBE_ATTEMPTS && !bq27220_present; attempt++) {
        if(attempt) vTaskDelay(pdMS_TO_TICKS(BQ27220_PROBE_RETRY_MS));
        if(bq27220_ack()) bq27220_identify_locked();
    }
    bq27220_last_probe_tick = xTaskGetTickCount();
    xSemaphoreGive(bq27220_mutex);

    if(!bq27220_present) {
        ESP_LOGW(
            TAG,
            "BQ27220 not found at 0x%02X after %d attempts (no battery?); retrying every %us",
            BQ27220_ADDR,
            BQ27220_PROBE_ATTEMPTS,
            (unsigned)(BQ27220_REPROBE_MS / 1000U));
    }
    return bq27220_present;
}

/* Late detection (battery inserted after boot, or the gauge was browning out
 * during the boot probe). One address probe at most every BQ27220_REPROBE_MS,
 * never blocking the caller on the mutex. */
static void bq27220_reprobe(void) {
    if(bq27220_present || !bq27220_initialized || !bq27220_mutex) return;
    const TickType_t now = xTaskGetTickCount();
    if((TickType_t)(now - bq27220_last_probe_tick) < pdMS_TO_TICKS(BQ27220_REPROBE_MS)) return;
    if(xSemaphoreTake(bq27220_mutex, 0) != pdTRUE) return;
    bq27220_last_probe_tick = now;
    if(!bq27220_present && bq27220_ack()) {
        bq27220_identify_locked();
    }
    xSemaphoreGive(bq27220_mutex);
}

bool furi_hal_bq27220_is_present(void) {
    bq27220_reprobe();
    return bq27220_present;
}

bool furi_hal_bq27220_is_data_valid(void) {
    FuriHalBq27220Snapshot s;
    if(!bq27220_snapshot_get(&s) || !s.battery_present) return false;
    return (TickType_t)(xTaskGetTickCount() - s.tick) < pdMS_TO_TICKS(BQ27220_DATA_TIMEOUT_MS);
}

bool furi_hal_bq27220_get_snapshot(FuriHalBq27220Snapshot* out) {
    return bq27220_snapshot_get(out);
}

uint8_t furi_hal_bq27220_get_charge_pct(void) {
    FuriHalBq27220Snapshot s;
    if(!bq27220_snapshot_get(&s)) return 0;
    return (uint8_t)s.state_of_charge;
}

bool furi_hal_bq27220_is_charging(void) {
    FuriHalBq27220Snapshot s;
    if(!bq27220_snapshot_get(&s)) return false;
    /* DSG bit (bit 0): 1 = discharging, 0 = charging/relaxation */
    return (s.battery_status & FURI_HAL_BQ27220_STATUS_DSG) == 0;
}

bool furi_hal_bq27220_is_full_charged(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) && (s.battery_status & FURI_HAL_BQ27220_STATUS_FC);
}

bool furi_hal_bq27220_is_sysdown(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) && s.fresh && s.battery_present &&
           (s.battery_status & FURI_HAL_BQ27220_STATUS_SYSDWN);
}

uint16_t furi_hal_bq27220_get_voltage_mv(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.voltage_mv : 0;
}

int16_t furi_hal_bq27220_get_current_ma(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.current_ma : 0;
}

int16_t furi_hal_bq27220_get_average_current_ma(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.average_current_ma : 0;
}

uint8_t furi_hal_bq27220_get_health_pct(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? (uint8_t)s.state_of_health : 0;
}

uint16_t furi_hal_bq27220_get_temperature_raw(void) {
    FuriHalBq27220Snapshot s;
    /* 2981 = 25.0 C, so a missing reading never shows up as -273 C. */
    return bq27220_snapshot_get(&s) ? s.temperature_dk : 2981U;
}

uint16_t furi_hal_bq27220_get_remaining_capacity_mah(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.remaining_capacity_mah : 0;
}

uint16_t furi_hal_bq27220_get_full_charge_capacity_mah(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.full_charge_capacity_mah : 0;
}

uint16_t furi_hal_bq27220_get_design_capacity_mah(void) {
    return bq27220_present ? bq27220_design_capacity_mah : 0;
}

uint16_t furi_hal_bq27220_get_time_to_empty_min(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.time_to_empty_min : FURI_HAL_BQ27220_TIME_NONE;
}

uint16_t furi_hal_bq27220_get_time_to_full_min(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.time_to_full_min : FURI_HAL_BQ27220_TIME_NONE;
}

uint16_t furi_hal_bq27220_get_cycle_count(void) {
    FuriHalBq27220Snapshot s;
    return bq27220_snapshot_get(&s) ? s.cycle_count : 0;
}

uint16_t furi_hal_bq27220_get_device_number(void) {
    return bq27220_device_number;
}

uint16_t furi_hal_bq27220_get_fw_version(void) {
    return bq27220_fw_version;
}

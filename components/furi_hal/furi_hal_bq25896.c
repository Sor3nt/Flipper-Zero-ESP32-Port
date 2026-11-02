/**
 * @file furi_hal_bq25896.c
 * BQ25896 charger driver — ESP-IDF I2C (legacy driver API)
 * Ported from STM32 Flipper firmware (lib/drivers/bq25896.c).
 *
 * I2C address: 0x6B
 * Bus: Qwiic/Grove (shared with BQ27220)
 *
 * Perubahan vs versi sebelumnya:
 *  - Semua read-modify-write lewat bq25896_update_reg(): TIDAK menulis bila read gagal
 *    (dulu nilai 0 ditulis balik -> EN_TERM/EN_TIMER hilang, SYS_MIN reset, dst).
 *  - RMW diserialkan mutex (task lain tidak bisa menyisip di antara read dan write).
 *  - set_vreg dipanggil SETELAH bq25896_present=true (dulu no-op diam-diam saat init).
 *  - is_charging() memasukkan "charge done" (sama dengan STM32); status di-cache
 *    sehingga error I2C sesaat tidak membuat status berkedip.
 *  - disable_otg() mengembalikan CHG_CONFIG (dulu charging mati permanen setelah OTG).
 *  - VREG dibatasi BQ25896_VREG_MAX_MV (default 4208 mV) agar caller tidak bisa overcharge.
 *  - prepare_for_sleep() mematikan ADC kontinu (hemat arus saat deep sleep).
 *  - poweroff(): tunggu terbatas setelah BATFET_DIS; bila masih hidup, pulihkan
 *    BATFET/charging dan return false (dulu loop tak terbatas sampai WDT reset).
 *  - I2C: transaksi tanpa malloc (write_read_device), timeout 100 ms (dulu 1 s).
 */

#include "furi_hal_bq25896.h"
#include "boards/board.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdint.h>

static const char* TAG = "BQ25896";

#define BQ25896_ADDR   0x6B
#define BQ_I2C_PORT    I2C_NUM_0
#define BQ_I2C_TIMEOUT pdMS_TO_TICKS(100)
#define BQ_I2C_RETRIES 2

#ifndef BQ25896_CHARGE_LIMIT
#define BQ25896_CHARGE_LIMIT 1200 /* mA */
#endif
#ifndef BQ25896_VREG_MV
#define BQ25896_VREG_MV 4208
#endif
#ifndef BQ25896_VREG_MAX_MV
#define BQ25896_VREG_MAX_MV 4208 /* batas keras sel Li-ion 4.2V */
#endif

/* Key registers */
#define REG00 0x00 /* Input current limit, EN_HIZ */
#define REG02 0x02 /* ADC control: CONV_START, CONV_RATE, ICO_EN */
#define REG03 0x03 /* SYS config: OTG, CHG enable */
#define REG04 0x04 /* Fast charge current limit */
#define REG05 0x05 /* Pre-charge / termination current */
#define REG06 0x06 /* Charge voltage limit (VREG) */
#define REG07 0x07 /* Charge timer, watchdog */
#define REG09 0x09 /* BATFET control (ship mode) */
#define REG0B 0x0B /* Status: VBUS, CHG, PG */
#define REG0E 0x0E /* ADC: battery voltage */
#define REG10 0x10 /* ADC: NTC percentage */
#define REG11 0x11 /* ADC: VBUS voltage */
#define REG12 0x12 /* ADC: charge current */
#define REG13 0x13 /* IINDPM/VINDPM status */
#define REG14 0x14 /* Reset, device ID, ICO_OPTIMIZED */

#define REG02_ADC_CONT      0xC0U /* CONV_START | CONV_RATE */
#define REG02_ICO_EN        (1U << 4)
#define REG03_CHG_CONFIG    0x10U
#define REG03_OTG_CONFIG    0x20U
#define REG09_BATFET_DIS    (1U << 5)
#define REG09_BATFET_DLY    (1U << 3)
#define REG09_BATFET_RST_EN (1U << 2)

/* IPRECHG=128mA (bits 7:4 = 0001), ITERM=64mA (bits 3:0 = 0000). Seluruh 8 bit terdefinisi,
 * jadi penulisan langsung aman tanpa read. */
#define REG05_VALUE 0x10U

static volatile bool bq25896_present = false;
static bool bq25896_charge_wanted = true; /* niat caller; dipulihkan setelah OTG */
static volatile uint8_t bq25896_last_chrg_stat = 0;
static volatile TickType_t bq25896_chrg_stat_tick = 0;
static bool bq25896_vbus_log_valid = false;
static bool bq25896_vbus_log_present = false;
static bool bq25896_vbus_error_logged = false;
static SemaphoreHandle_t bq25896_mutex = NULL;

/* ---------------------------------------------------------------- low level */

static bool bq25896_read_reg(uint8_t reg, uint8_t* val) {
    for(int i = 0; i < BQ_I2C_RETRIES; i++) {
        if(i2c_master_write_read_device(
               BQ_I2C_PORT, BQ25896_ADDR, &reg, 1, val, 1, BQ_I2C_TIMEOUT) == ESP_OK) {
            return true;
        }
    }
    return false;
}

static bool bq25896_write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    for(int i = 0; i < BQ_I2C_RETRIES; i++) {
        if(i2c_master_write_to_device(BQ_I2C_PORT, BQ25896_ADDR, buf, sizeof(buf), BQ_I2C_TIMEOUT) ==
           ESP_OK) {
            return true;
        }
    }
    return false;
}

static bool bq25896_lock(void) {
    if(!bq25896_mutex) return true; /* sebelum init / alloc gagal: tanpa lock */
    return xSemaphoreTake(bq25896_mutex, pdMS_TO_TICKS(300)) == pdTRUE;
}

static void bq25896_unlock(void) {
    if(bq25896_mutex) xSemaphoreGive(bq25896_mutex);
}

/* Atomic read-modify-write. Tidak menulis apa pun bila read gagal. */
static bool bq25896_update_reg(uint8_t reg, uint8_t mask, uint8_t val) {
    if(!bq25896_lock()) return false;
    uint8_t v = 0;
    bool ok = bq25896_read_reg(reg, &v);
    if(ok) {
        v = (uint8_t)((v & (uint8_t)~mask) | (val & mask));
        ok = bq25896_write_reg(reg, v);
    }
    bq25896_unlock();
    return ok;
}

/* CHRG_STAT: 00=Not charging, 01=Pre-charge, 10=Fast charge, 11=Charge done.
 * Error I2C sesaat -> pakai nilai terakhir; kedaluwarsa setelah 3 s. */
static uint8_t bq25896_chrg_stat(void) {
    uint8_t r = 0;
    const TickType_t now = xTaskGetTickCount();
    /* is_charging() and is_charging_done() are queried back to back every
     * power tick; one REG0B read serves both. */
    if(bq25896_chrg_stat_tick && (TickType_t)(now - bq25896_chrg_stat_tick) < pdMS_TO_TICKS(500)) {
        return bq25896_last_chrg_stat;
    }
    if(bq25896_read_reg(REG0B, &r)) {
        bq25896_last_chrg_stat = (uint8_t)((r >> 3) & 0x03U);
        bq25896_chrg_stat_tick = now;
    } else if((TickType_t)(now - bq25896_chrg_stat_tick) > pdMS_TO_TICKS(3000)) {
        bq25896_last_chrg_stat = 0;
    }
    return bq25896_last_chrg_stat;
}

/* --------------------------------------------------------------------- init */

bool furi_hal_bq25896_init(void) {
    /* I2C bus assumed already initialized by power HAL */
    if(!bq25896_mutex) {
        bq25896_mutex = xSemaphoreCreateMutex();
        if(!bq25896_mutex) ESP_LOGW(TAG, "Mutex alloc failed; register RMW not serialized");
    }

    uint8_t reg14 = 0;
    if(!bq25896_read_reg(REG14, &reg14)) {
        ESP_LOGW(TAG, "BQ25896 not found at 0x%02X", BQ25896_ADDR);
        return false;
    }

    /* REG14[5:3] PN = 000 for bq25896, REG14[1:0] DEV_REV = 10 (SLUSC76C table 26). */
    const uint8_t dev_id = (reg14 >> 3) & 0x07;
    ESP_LOGI(
        TAG, "BQ25896 found: REG14=0x%02X PN=%u DEV_REV=%u", reg14, dev_id, (unsigned)(reg14 & 0x03U));
    if(dev_id != 0x00) {
        ESP_LOGW(TAG, "Unexpected PN=%u (bq25896 = 0); continuing", dev_id);
    }

    /* Wajib sebelum memanggil helper publik (semuanya early-return bila !present). */
    bq25896_present = true;

    /* Skip hard reset — preserves charger state across reboots (bus I2C dipakai bersama). */

    /* ADC kontinu (CONV_START=1, CONV_RATE=1) + ICO_EN=1 (eksplisit, jangan andalkan default). */
    if(!bq25896_update_reg(REG02, REG02_ADC_CONT | REG02_ICO_EN, REG02_ADC_CONT | REG02_ICO_EN)) {
        ESP_LOGW(TAG, "REG02 update failed");
    }

    /* Watchdog off (REG07 bits 5:4 = 00). EN_TERM/EN_TIMER tidak tersentuh. */
    if(!bq25896_update_reg(REG07, 0x30U, 0x00U)) {
        ESP_LOGW(TAG, "REG07 update failed");
    }

    furi_hal_bq25896_set_vreg_voltage_mv(BQ25896_VREG_MV);

    /* ICHG: 64 mA/step, bits 6:0. Bit 7 (EN_PUMPX) dipertahankan. Maks chip 5056 mA. */
    uint32_t ichg = (uint32_t)(BQ25896_CHARGE_LIMIT) / 64U;
    if(ichg < 1U) ichg = 1U;
    if(ichg > 79U) ichg = 79U;
    if(!bq25896_update_reg(REG04, 0x7FU, (uint8_t)ichg)) {
        ESP_LOGW(TAG, "Failed to write REG04; keeping charger current limit");
    }

    if(!bq25896_write_reg(REG05, REG05_VALUE)) {
        ESP_LOGW(TAG, "REG05 write failed");
    }

    /* Satu RMW: OTG_CONFIG=0 dan CHG_CONFIG=1 (jangan mewarisi boost dari state lama). */
    bq25896_charge_wanted = true;
    if(!bq25896_update_reg(REG03, REG03_OTG_CONFIG | REG03_CHG_CONFIG, REG03_CHG_CONFIG)) {
        ESP_LOGW(TAG, "REG03 update failed; charging state unknown");
    }

    uint8_t r02 = 0, r04 = 0, r05 = 0;
    bq25896_read_reg(REG02, &r02);
    bq25896_read_reg(REG04, &r04);
    bq25896_read_reg(REG05, &r05);
    ESP_LOGI(
        TAG,
        "Config: VREG=%umV ICHG=%umA IPRECHG=%umA ITERM=%umA ICO=%s ADC=%s",
        furi_hal_bq25896_get_vreg_voltage_mv(),
        (unsigned)(r04 & 0x7F) * 64U,
        (unsigned)((r05 >> 4) & 0x0F) * 64U + 64U,
        (unsigned)(r05 & 0x0F) * 64U + 64U,
        (r02 & REG02_ICO_EN) ? "on" : "off",
        (r02 & 0x40U) ? "cont" : "off");
    return true;
}

bool furi_hal_bq25896_is_present(void) {
    return bq25896_present;
}

/* ------------------------------------------------------------------- status */

bool furi_hal_bq25896_is_charging(void) {
    if(!bq25896_present) return false;
    /* Pre-charge, fast charge, dan charge-done semuanya dihitung "charging"
     * (sama dengan bq25896_is_charging() di STM32). Power service memakai
     * is_charging() && is_charging_done() untuk state "charged". */
    return bq25896_chrg_stat() != 0x00U;
}

bool furi_hal_bq25896_is_charging_done(void) {
    if(!bq25896_present) return false;
    return bq25896_chrg_stat() == 0x03U;
}

void furi_hal_bq25896_enable_charging(void) {
    if(!bq25896_present) return;
    bq25896_charge_wanted = true;
    /* Saat OTG aktif, jangan hidupkan charging; disable_otg() yang memulihkan. */
    if(!bq25896_update_reg(REG03, REG03_CHG_CONFIG, REG03_CHG_CONFIG)) {
        ESP_LOGW(TAG, "enable_charging: REG03 update failed");
    }
}

void furi_hal_bq25896_disable_charging(void) {
    if(!bq25896_present) return;
    bq25896_charge_wanted = false;
    if(!bq25896_update_reg(REG03, REG03_CHG_CONFIG, 0x00U)) {
        ESP_LOGW(TAG, "disable_charging: REG03 update failed");
    }
}

uint16_t furi_hal_bq25896_get_vbus_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg11 = 0;
    if(!bq25896_read_reg(REG11, &reg11)) return 0;
    if((reg11 & 0x80U) == 0) {
        /* VBUS_GD=0: nilai VBUSV tidak bermakna (offset 2600 mV jangan tampil sebagai 2.6 V). */
        return 0;
    }
    /* VBUS = 2600mV + REG11[6:0] x 100mV */
    return (uint16_t)(2600U + (reg11 & 0x7FU) * 100U);
}

uint16_t furi_hal_bq25896_get_vbat_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg0e = 0;
    if(!bq25896_read_reg(REG0E, &reg0e)) return 0;
    /* VBAT = 2304mV + REG0E[6:0] x 20mV */
    return (uint16_t)(2304U + (reg0e & 0x7FU) * 20U);
}

uint16_t furi_hal_bq25896_get_vbat_current_ma(void) {
    if(!bq25896_present) return 0;
    uint8_t reg12 = 0;
    if(!bq25896_read_reg(REG12, &reg12)) return 0;
    /* ICHG = REG12[6:0] x 50mA */
    return (uint16_t)((reg12 & 0x7FU) * 50U);
}

int32_t furi_hal_bq25896_get_temperature_mc(void) {
    if(!bq25896_present) return 25000; /* default 25°C */
    uint8_t reg10 = 0;
    if(!bq25896_read_reg(REG10, &reg10)) return 25000;
    /* TSPCT = 21% + REG10[6:0] x 0.465%; aproksimasi STM32: (71 - NTCPCT) / 0.6 */
    const float ntc_pct = 21.0f + (float)(reg10 & 0x7F) * 0.465f;
    const float temp_c = (71.0f - ntc_pct) / 0.6f;
    return (int32_t)(temp_c * 1000.0f);
}

uint16_t furi_hal_bq25896_get_vreg_voltage_mv(void) {
    if(!bq25896_present) return 0;
    uint8_t reg06 = 0;
    if(!bq25896_read_reg(REG06, &reg06)) return 0;
    /* VREG = 3840mV + REG06[7:2] x 16mV */
    return (uint16_t)(3840U + ((reg06 >> 2) & 0x3FU) * 16U);
}

void furi_hal_bq25896_set_vreg_voltage_mv(uint16_t vreg_mv) {
    if(!bq25896_present) return;
    if(vreg_mv < 3840) vreg_mv = 3840;
    if(vreg_mv > BQ25896_VREG_MAX_MV) vreg_mv = BQ25896_VREG_MAX_MV;
    uint32_t code = ((uint32_t)vreg_mv - 3840U) / 16U;
    if(code > 0x3FU) code = 0x3FU;
    /* Bit 1:0 (BATLOWV, VRECHG) dipertahankan. */
    if(!bq25896_update_reg(REG06, 0xFCU, (uint8_t)(code << 2))) {
        ESP_LOGW(TAG, "VREG update failed");
    }
}

/* ---------------------------------------------------------------------- OTG */

void furi_hal_bq25896_enable_otg(void) {
    if(!bq25896_present) return;
    /* OTG_CONFIG=1, CHG_CONFIG=0 dalam satu RMW. */
    if(!bq25896_update_reg(REG03, REG03_OTG_CONFIG | REG03_CHG_CONFIG, REG03_OTG_CONFIG)) {
        ESP_LOGW(TAG, "enable_otg: REG03 update failed");
    }
}

void furi_hal_bq25896_disable_otg(void) {
    if(!bq25896_present) return;
    /* OTG_CONFIG=0 dan pulihkan CHG_CONFIG sesuai niat terakhir caller.
     * Dulu bit ini tidak dipulihkan -> charging mati sampai reboot. */
    const uint8_t chg = bq25896_charge_wanted ? REG03_CHG_CONFIG : 0x00U;
    if(!bq25896_update_reg(REG03, REG03_OTG_CONFIG | REG03_CHG_CONFIG, chg)) {
        ESP_LOGW(TAG, "disable_otg: REG03 update failed");
    }
}

bool furi_hal_bq25896_is_otg_enabled(void) {
    if(!bq25896_present) return false;
    uint8_t reg03 = 0;
    if(!bq25896_read_reg(REG03, &reg03)) return false;
    return (reg03 & REG03_OTG_CONFIG) != 0;
}

/* --------------------------------------------------------------------- VBUS */

bool furi_hal_bq25896_is_vbus_present(void) {
    bool present = false;
    return furi_hal_bq25896_get_vbus_present(&present) && present;
}

bool furi_hal_bq25896_get_vbus_present(bool* present) {
    if(!present || !bq25896_present) return false;
    uint8_t reg0b = 0;
    uint8_t reg11 = 0;
    bool read_ok = false;
    for(uint8_t attempt = 0; attempt < 3; attempt++) {
        if(bq25896_read_reg(REG0B, &reg0b) && bq25896_read_reg(REG11, &reg11)) {
            read_ok = true;
            break;
        }
        vTaskDelay(1);
    }
    if(!read_ok) {
        if(!bq25896_vbus_error_logged) {
            ESP_LOGE(TAG, "VBUS detection failed: charger status read error");
            bq25896_vbus_error_logged = true;
        }
        return false;
    }
    bq25896_vbus_error_logged = false;
    /* VBUS_GD = indikasi VBUS good. 3.5 V adalah VSYS min, bukan ambang VBUS. */
    const bool vbus_good = (reg11 & 0x80U) != 0;
    const uint16_t vbus_mv = (uint16_t)(2600U + (reg11 & 0x7FU) * 100U);
    *present = vbus_good;
    if(!bq25896_vbus_log_valid || bq25896_vbus_log_present != vbus_good) {
        ESP_LOGI(
            TAG,
            "VBUS state changed: REG0B=0x%02X VBUS_STAT=%u REG11=0x%02X "
            "VBUS_GD=%d VBUS_ADC=%umV",
            reg0b,
            (unsigned)((reg0b >> 5) & 0x07U),
            reg11,
            vbus_good,
            (unsigned)vbus_mv);
        bq25896_vbus_log_valid = true;
        bq25896_vbus_log_present = vbus_good;
    }
    return true;
}

bool furi_hal_bq25896_read_vbus_good(bool* present) {
    if(!present || !bq25896_present) return false;
    /* VBUS_GD comes from the input-source qualification, not from the battery
     * monitor ADC, so it stays valid while the ADC is off (SLUSC76C 9.2.3.2). */
    uint8_t reg11 = 0;
    if(!bq25896_read_reg(REG11, &reg11)) return false;
    *present = (reg11 & 0x80U) != 0;
    return true;
}

bool furi_hal_bq25896_set_adc_continuous(bool enable) {
    if(!bq25896_present) return false;
    /* CONV_RATE=1 runs a conversion every second; clearing it (and CONV_START)
     * stops the battery monitor so REGN does not add quiescent current. */
    const bool ok = bq25896_update_reg(REG02, REG02_ADC_CONT, enable ? REG02_ADC_CONT : 0x00U);
    if(!ok) ESP_LOGW(TAG, "ADC %s failed", enable ? "enable" : "disable");
    return ok;
}

bool furi_hal_bq25896_get_dpm_status(bool* vdpm, bool* idpm, uint16_t* iinlim_ma, bool* ico_done) {
    if(!bq25896_present || !vdpm || !idpm || !iinlim_ma || !ico_done) return false;
    uint8_t r13 = 0, r14 = 0;
    if(!bq25896_read_reg(REG13, &r13) || !bq25896_read_reg(REG14, &r14)) return false;
    *vdpm = (r13 & 0x80U) != 0; /* VINDPM aktif: sumber VBUS lemah/jatuh */
    *idpm = (r13 & 0x40U) != 0; /* IINDPM aktif: input current dibatasi */
    *iinlim_ma = (uint16_t)(100U + (r13 & 0x3FU) * 50U); /* limit efektif (hasil ICO bila selesai) */
    *ico_done = (r14 & 0x40U) != 0; /* ICO_OPTIMIZED */
    return true;
}

bool furi_hal_bq25896_set_ico(bool enable) {
    if(!bq25896_present) return false;
    return bq25896_update_reg(REG02, REG02_ICO_EN, enable ? REG02_ICO_EN : 0x00U);
}

/* -------------------------------------------------------------- sleep / off */

bool furi_hal_bq25896_prepare_for_sleep(void) {
    if(!bq25896_present) return false;
    for(uint8_t attempt = 0; attempt < 3; attempt++) {
        bool ok = false;
        if(bq25896_lock()) {
            uint8_t r00 = 0, r02 = 0, r03 = 0, r07 = 0;
            if(bq25896_read_reg(REG00, &r00) && bq25896_read_reg(REG02, &r02) &&
               bq25896_read_reg(REG03, &r03) && bq25896_read_reg(REG07, &r07)) {
                r00 &= (uint8_t)~0x80U; /* EN_HIZ=0: input tidak boleh high-Z */
                r02 &= (uint8_t)~REG02_ADC_CONT; /* ADC kontinu off: hemat arus saat tidur */
                r03 &= (uint8_t)~REG03_OTG_CONFIG; /* boost tidak menyala saat tidur */
                r03 |= REG03_CHG_CONFIG; /* charging tetap jalan otonom */
                r07 &= (uint8_t)~0x30U; /* watchdog off: bila expire register reset (ICHG 2A) */
                if(bq25896_write_reg(REG03, r03) && bq25896_write_reg(REG00, r00) &&
                   bq25896_write_reg(REG07, r07) && bq25896_write_reg(REG02, r02)) {
                    uint8_t c00 = 0, c02 = 0, c03 = 0, c07 = 0;
                    if(bq25896_read_reg(REG00, &c00) && bq25896_read_reg(REG02, &c02) &&
                       bq25896_read_reg(REG03, &c03) && bq25896_read_reg(REG07, &c07) &&
                       (c00 & 0x80U) == 0 && (c02 & 0x40U) == 0 && (c03 & 0x30U) == 0x10U &&
                       (c07 & 0x30U) == 0) {
                        ok = true;
                    }
                }
            }
            bq25896_unlock();
        }
        if(ok) {
            ESP_LOGI(
                TAG, "Sleep prep verified: EN_HIZ=0 CHG_CONFIG=1 OTG=0 WATCHDOG=off ADC=off");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGW(TAG, "Sleep prep verification failed after 3 attempts");
    return false;
}

/* Dipanggil dengan mutex dipegang. Sukses = MCU kehilangan daya, jadi fungsi ini tidak
 * pernah kembali. Semua jalur return berarti GAGAL dan state charger sudah dipulihkan. */
static bool bq25896_ship_mode_locked(void) {
    uint8_t reg0b_before = 0;
    uint8_t reg11_before = 0;
    uint8_t reg09 = 0;
    if(!bq25896_read_reg(REG0B, &reg0b_before) || !bq25896_read_reg(REG11, &reg11_before) ||
       !bq25896_read_reg(REG09, &reg09)) {
        ESP_LOGE(TAG, "BATFET shutdown: failed to read charger state");
        return false;
    }

    if((reg11_before & 0x80U) != 0) {
        ESP_LOGW(
            TAG,
            "BATFET shutdown refused: VBUS_GD=1 REG11=0x%02X VBUS_STAT=%u",
            reg11_before,
            (unsigned)((reg0b_before >> 5) & 0x07U));
        return false;
    }
    ESP_LOGI(
        TAG,
        "BATFET shutdown begin: REG0B=0x%02X VBUS_STAT=%u REG11=0x%02X VBUS_GD=0 REG09=0x%02X",
        reg0b_before,
        (unsigned)((reg0b_before >> 5) & 0x07),
        reg11_before,
        reg09);

    /* Charger/boost jangan menggerakkan SYS saat perintah BATFET diterapkan. */
    uint8_t reg03 = 0;
    if(!bq25896_read_reg(REG03, &reg03)) {
        ESP_LOGE(TAG, "BATFET shutdown: failed to read REG03");
        return false;
    }
    const uint8_t reg03_off = (uint8_t)(reg03 & (uint8_t)~(REG03_CHG_CONFIG | REG03_OTG_CONFIG));
    ESP_LOGI(TAG, "Shutdown I2C TX: REG03 <= 0x%02X (charger/OTG disabled)", reg03_off);
    if(!bq25896_write_reg(REG03, reg03_off)) {
        ESP_LOGE(TAG, "BATFET shutdown: failed to disable charger/OTG");
        return false;
    }

    uint8_t reg07 = 0;
    if(!bq25896_read_reg(REG07, &reg07)) {
        ESP_LOGE(TAG, "BATFET shutdown: failed to read REG07");
        bq25896_write_reg(REG03, reg03); /* pulihkan */
        return false;
    }
    reg07 &= (uint8_t)~0x30U;
    if(!bq25896_write_reg(REG07, reg07)) {
        ESP_LOGE(TAG, "BATFET shutdown: failed to disable watchdog");
        bq25896_write_reg(REG03, reg03);
        return false;
    }

    /* REG09 = transaksi I2C terakhir: BATFET putus seketika, /QON auto-reset off. */
    const uint8_t reg09_off =
        (uint8_t)((reg09 & (uint8_t)~(REG09_BATFET_DLY | REG09_BATFET_RST_EN)) | REG09_BATFET_DIS);
    ESP_LOGI(
        TAG,
        "Shutdown I2C TX: REG09 <= 0x%02X (BATFET_DIS=1, BATFET_DLY=0, BATFET_RST_EN=0)",
        reg09_off);
    for(uint8_t attempt = 0; attempt < 3; attempt++) {
        ESP_LOGI(TAG, "Shutdown I2C attempt %u/3: write REG09", (unsigned)(attempt + 1));
        if(bq25896_write_reg(REG09, reg09_off)) {
            /* Mulai sini: tanpa log, tanpa read-back, tanpa akses periferal lain.
             * Tunggu terbatas (150 ms < interrupt WDT 300 ms) agar SYS sempat runtuh. */
            const UBaseType_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
            esp_rom_delay_us(150000);
            portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);

            /* Masih hidup = BATFET tidak memutus SYS (mis. VBUS muncul saat proses).
             * Batalkan supaya baterai tidak terlepas dan charging kembali. */
            bq25896_write_reg(REG09, (uint8_t)(reg09 & (uint8_t)~REG09_BATFET_DIS));
            bq25896_write_reg(
                REG03,
                (uint8_t)(reg03_off | (bq25896_charge_wanted ? REG03_CHG_CONFIG : 0x00U)));
            ESP_LOGE(TAG, "BATFET shutdown: system still alive after BATFET_DIS; reverted");
            return false;
        }
        ESP_LOGW(TAG, "Shutdown I2C write failed for REG09");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "BATFET shutdown: final REG09 write failed (0x%02X)", reg09_off);
    bq25896_write_reg(REG03, reg03); /* charger/OTG kembali seperti semula */
    return false;
}

bool furi_hal_bq25896_poweroff(void) {
    if(!bq25896_present) return false;
    /* Ship mode: BATFET off (REG09 bit 5) memutus baterai dari rail sistem = power-off
     * sesungguhnya (0 draw). Bangun hanya via USB baru atau tombol /QON. Dengan VBUS ada
     * SYS tetap hidup, jadi fungsi ini menolak bila VBUS_GD=1. Pada jalur sukses fungsi
     * ini tidak pernah kembali (MCU kehilangan daya); return false = gagal/dibatalkan. */
    if(!bq25896_lock()) {
        ESP_LOGE(TAG, "BATFET shutdown: I2C lock timeout");
        return false;
    }
    const bool ok = bq25896_ship_mode_locked();
    bq25896_unlock();
    return ok;
}

#pragma once

/* Cached CC1101 tuning for repeated RSSI scans. Calibration results belong to
 * the caller and must be accessed by one scanner task at a time. */
#include <lib/drivers/cc1101.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <furi.h>
#include <string.h>

#define SUBGHZ_RSSI_SCAN_SPI_TIMEOUT 50
#define SUBGHZ_RSSI_SCAN_RSSI_NONE   (-127.0f)

typedef struct {
    /* Keep the requested channel as the cache key. `frequency` is the actual
     * synthesizer frequency after CC1101 word quantization. */
    uint32_t requested_frequency;
    uint32_t frequency;
    uint8_t freq[3];
    uint8_t fscal[3];
    bool valid;
} SubGhzRssiScanPoint;

static inline bool subghz_rssi_scan_burst(uint8_t address, uint8_t* data, size_t size) {
    uint8_t tx[4] = {0};
    uint8_t rx[4] = {0};
    furi_check(size <= 3);
    tx[0] = address | CC1101_BURST;
    memcpy(&tx[1], data, size);
    if(!furi_hal_spi_bus_trx(
           &furi_hal_spi_bus_handle_subghz, tx, rx, size + 1, SUBGHZ_RSSI_SCAN_SPI_TIMEOUT)) {
        return false;
    }
    memcpy(data, &rx[1], size);
    return true;
}

static inline void subghz_rssi_scan_set_frequency(SubGhzRssiScanPoint* point, uint32_t frequency) {
    const uint64_t word = ((uint64_t)frequency * CC1101_FDIV / CC1101_QUARTZ) & CC1101_FMASK;
    point->requested_frequency = frequency;
    point->freq[0] = (word >> 16) & 0xFF;
    point->freq[1] = (word >> 8) & 0xFF;
    point->freq[2] = word & 0xFF;
    point->frequency = (uint32_t)(word * CC1101_QUARTZ / CC1101_FDIV);
}

/* Calibrate a point once. Later hops restore the cached FSCAL3..1 values. */
static inline bool subghz_rssi_scan_calibrate(uint32_t frequency, SubGhzRssiScanPoint* point) {
    point->valid = false;
    subghz_rssi_scan_set_frequency(point, frequency);
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
    bool ok = cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 1000);
    if(ok) {
        uint8_t freq[3] = {point->freq[0], point->freq[1], point->freq[2]};
        ok = subghz_rssi_scan_burst(CC1101_FREQ2, freq, sizeof(freq));
    }
    if(ok) {
        cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
        ok = cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 10000);
    }
    if(ok) {
        ok = subghz_rssi_scan_burst(CC1101_FSCAL3 | CC1101_READ, point->fscal, sizeof(point->fscal));
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    point->valid = ok;
    return ok;
}

static inline bool subghz_rssi_scan_tune(const SubGhzRssiScanPoint* point) {
    if(!point->valid) return false;
    uint8_t freq[3] = {point->freq[0], point->freq[1], point->freq[2]};
    uint8_t fscal[3] = {point->fscal[0], point->fscal[1], point->fscal[2]};
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
    bool ok = cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateIDLE, 1000);
    if(ok) {
        ok = subghz_rssi_scan_burst(CC1101_FREQ2, freq, sizeof(freq)) &&
             subghz_rssi_scan_burst(CC1101_FSCAL3, fscal, sizeof(fscal));
    }
    if(ok) {
        cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
        ok = cc1101_wait_status_state(&furi_hal_spi_bus_handle_subghz, CC1101StateRX, 2000);
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    return ok;
}

static inline uint32_t subghz_rssi_scan_settle_us(uint32_t filter_bw_hz, uint32_t scale) {
    uint32_t us = filter_bw_hz ? scale / filter_bw_hz : 1200;
    if(us < 300) us = 300;
    if(us > 1200) us = 1200;
    return us;
}

static inline float subghz_rssi_scan_measure(uint32_t settle_us, uint8_t samples) {
    furi_delay_us(settle_us);
    float peak = SUBGHZ_RSSI_SCAN_RSSI_NONE;
    for(uint8_t i = 0; i < samples; i++) {
        const float rssi = furi_hal_subghz_get_rssi();
        if(rssi > peak) peak = rssi;
    }
    return peak;
}

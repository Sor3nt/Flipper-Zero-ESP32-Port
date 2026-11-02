#pragma once

/* Fast CC1101 RSSI scanning, shared by the Spectrum and Frequency Analyzer
 * workers.
 *
 * A manual calibration (SCAL) costs ~0.8 ms plus several SPI round trips on
 * every frequency change, and the radio SPI on shared-bus boards runs slowly.
 * Both scanners revisit the same frequencies over and over, so each frequency
 * is calibrated once, FSCAL3..FSCAL1 are cached, and every later visit just
 * restores them (CC1101 datasheet, 28.2 "Frequency Hopping", option 2):
 * SIDLE + two 4-byte register bursts + SRX. MCSM0.FS_AUTOCAL must stay 0
 * (the reset default), which every scanner guarantees by resetting the chip
 * before use. */

#include <lib/drivers/cc1101.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <furi.h>
#include <string.h>

#define SUBGHZ_RSSI_SCAN_SPI_TIMEOUT 50
#define SUBGHZ_RSSI_SCAN_RSSI_NONE   (-127.0f)

typedef struct {
    uint32_t frequency; // real (synthesizer-rounded) frequency, Hz
    uint8_t freq[3]; // FREQ2..FREQ0
    uint8_t fscal[3]; // FSCAL3..FSCAL1
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
    // For a read burst the register values come back after the header byte.
    memcpy(data, &rx[1], size);
    return true;
}

/** Point `point` at `frequency` without touching its cached calibration.
 * Only valid for small moves: the fine scans use it to step a few hundred kHz
 * around a calibrated frequency, well inside one VCO calibration. */
static inline void subghz_rssi_scan_set_frequency(SubGhzRssiScanPoint* point, uint32_t frequency) {
    const uint64_t word = ((uint64_t)frequency * CC1101_FDIV / CC1101_QUARTZ) & CC1101_FMASK;
    point->freq[0] = (word >> 16) & 0xFF;
    point->freq[1] = (word >> 8) & 0xFF;
    point->freq[2] = word & 0xFF;
    point->frequency = (uint32_t)(word * CC1101_QUARTZ / CC1101_FDIV);
}

/** Calibrate `frequency` once and cache everything needed to return to it.
 * Leaves the radio in IDLE. */
static inline bool subghz_rssi_scan_calibrate(uint32_t frequency, SubGhzRssiScanPoint* point) {
    point->valid = false;
    subghz_rssi_scan_set_frequency(point, frequency);

    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
    bool ok = furi_hal_subghz_wait_for_state_locked(CC1101StateIDLE, 1000);
    if(ok) {
        uint8_t freq[3] = {point->freq[0], point->freq[1], point->freq[2]};
        ok = subghz_rssi_scan_burst(CC1101_FREQ2, freq, sizeof(freq));
    }
    if(ok) {
        cc1101_calibrate(&furi_hal_spi_bus_handle_subghz);
        ok = furi_hal_subghz_wait_for_state_locked(CC1101StateIDLE, 10000);
    }
    if(ok) {
        ok = subghz_rssi_scan_burst(CC1101_FSCAL3 | CC1101_READ, point->fscal, 3);
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);

    point->valid = ok;
    return ok;
}

/** Jump to a calibrated point and enter RX. */
static inline bool subghz_rssi_scan_tune(const SubGhzRssiScanPoint* point) {
    if(!point->valid) return false;

    uint8_t freq[3] = {point->freq[0], point->freq[1], point->freq[2]};
    uint8_t fscal[3] = {point->fscal[0], point->fscal[1], point->fscal[2]};

    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    cc1101_switch_to_idle(&furi_hal_spi_bus_handle_subghz);
    bool ok = subghz_rssi_scan_burst(CC1101_FREQ2, freq, sizeof(freq)) &&
              subghz_rssi_scan_burst(CC1101_FSCAL3, fscal, sizeof(fscal));
    if(ok) {
        cc1101_switch_to_rx(&furi_hal_spi_bus_handle_subghz);
        ok = furi_hal_subghz_wait_for_state_locked(CC1101StateRX, 2000);
    }
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
    return ok;
}

/** Time for the AGC/RSSI to settle after entering RX. It scales with the
 * inverse of the RX filter bandwidth (the channel filter decimates harder for
 * narrow filters, so each AGC step takes longer). */
static inline uint32_t subghz_rssi_scan_settle_us(uint32_t filter_bw_hz, uint32_t scale) {
    uint32_t settle_us = filter_bw_hz ? (uint32_t)(scale / filter_bw_hz) : 1200;
    if(settle_us < 300) settle_us = 300;
    if(settle_us > 1200) settle_us = 1200;
    return settle_us;
}

/** Wait `settle_us`, then take `samples` RSSI readings and return the peak.
 * Several readings make OOK bursts (carrier switching on/off at the bit rate)
 * far less likely to be sampled during an "off" gap. */
static inline float subghz_rssi_scan_measure(uint32_t settle_us, uint8_t samples) {
    furi_delay_us(settle_us);
    float peak = SUBGHZ_RSSI_SCAN_RSSI_NONE;
    for(uint8_t i = 0; i < samples; i++) {
        const float rssi = furi_hal_subghz_get_rssi();
        if(rssi > peak) peak = rssi;
    }
    return peak;
}

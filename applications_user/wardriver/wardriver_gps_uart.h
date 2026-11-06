#pragma once
/* Exclusive, resumable ownership of the board UART while GPS is listening.
 *
 * The USB-UART bridge, BW16 and the background RFID reader all take
 * the firmware's UART lease before they touch a driver. GPS used to rely on a bare
 * uart_is_driver_installed() check, which cannot see a different UART number on the
 * same pins, cannot make the RFID reader let go, and is not atomic with the install
 * that follows. Ownership order (undone in reverse by wardriver_gps_uart_close):
 *   1) furi_hal_uart_shared_acquire - serialize UART users. The background RFID
 *                                     reader sees the request, deletes its driver,
 *                                     restores the pins and releases the lease.
 *   2) furi_hal_shared_pins_acquire - GPIO43/44 only: the NRF/IR/RFID/BW16 header.
 *   3) driver check                 - never reconfigure a driver this code did not
 *                                     install; refuse before the pins are touched.
 *   4) furi_hal_shared_pins_save    - GPIO43/44 only: snapshot so stop restores it.
 *   5) light-sleep veto             - light sleep gates the UART clock and would drop
 *                                     NMEA bytes; entered just before the install and
 *                                     the last thing dropped, like the USB-UART bridge.
 *   6) UART driver on the requested port.
 * Every step records what it owns. A cleanup that fails keeps the remaining leases
 * and is finished by the next close/open instead of being forgotten, so a lease can
 * never be dropped while a driver or pin state it protects is still live.
 * Called from one thread only (the app worker), like the rest of the GPS module. */
#include <stdbool.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_uart_shared.h>
#include <furi_hal_shared_pins.h>

/* Same order of magnitude as USB-UART and BW16: long enough for the
 * RFID reader to notice the request (it polls every 50 ms) and release its driver. */
#define WARDRIVER_GPS_UART_LEASE_MS 1000
/* The RFID reader gives up the UART lease slightly before the pin lease. */
#define WARDRIVER_GPS_PINS_TRIES 40
#define WARDRIVER_GPS_PINS_TRY_MS 5
/* External UART header shared with NRF CE/CSN, IR TX, RFID and BW16. */
#define WARDRIVER_GPS_SHARED_TX 43
#define WARDRIVER_GPS_SHARED_RX 44

typedef enum {
    WardriverGpsUartOk,
    WardriverGpsUartBusy,     /* Another UART user owns a lease or a driver. */
    WardriverGpsUartPinsBusy, /* NRF/IR/other owns the GPIO43/44 lease. */
    WardriverGpsUartError,    /* Lease or driver setup failed; rolled back. */
    WardriverGpsUartCleanup,  /* A release failed; leases are kept for a retry. */
} WardriverGpsUartResult;

static struct {
    bool driver, uart_leased, pins_leased, pins_saved, sleep_inhibited;
    int uart, rx, tx;
} wardriver_gps_hw;

static bool wardriver_gps_pin_is_shared(int pin) {
    return pin == WARDRIVER_GPS_SHARED_TX || pin == WARDRIVER_GPS_SHARED_RX;
}

/* Idempotent. Returns true only when nothing is owned any more. */
static bool wardriver_gps_uart_close(void) {
    if(wardriver_gps_hw.driver) {
        esp_err_t error = uart_driver_delete(wardriver_gps_hw.uart);
        /* A driver that is still installed is still ours: keep every lease. */
        if(error != ESP_OK && uart_is_driver_installed(wardriver_gps_hw.uart)) return false;
        wardriver_gps_hw.driver = false;
        gpio_reset_pin(wardriver_gps_hw.rx);
        if(wardriver_gps_hw.tx >= 0) gpio_reset_pin(wardriver_gps_hw.tx);
    }
    /* Restore after the reset above and before the UART lease is dropped; the
     * lease cannot be released while the snapshot is outstanding. */
    if(wardriver_gps_hw.pins_saved) {
        furi_hal_shared_pins_restore(&wardriver_gps_hw);
        if(furi_hal_shared_pins_is_saved(&wardriver_gps_hw)) return false;
        wardriver_gps_hw.pins_saved = false;
    }
    if(wardriver_gps_hw.uart_leased) {
        if(!furi_hal_uart_shared_release()) return false;
        wardriver_gps_hw.uart_leased = false;
    }
    if(wardriver_gps_hw.pins_leased) {
        furi_hal_shared_pins_release(&wardriver_gps_hw);
        wardriver_gps_hw.pins_leased = false;
    }
    /* Last: a kept lease or driver above means GPS may still need the clock. */
    if(wardriver_gps_hw.sleep_inhibited) {
        furi_hal_power_light_sleep_inhibit_exit();
        wardriver_gps_hw.sleep_inhibited = false;
    }
    return true;
}

static bool wardriver_gps_acquire_pins(void) {
    for(unsigned i = 0; i < WARDRIVER_GPS_PINS_TRIES; ++i) {
        if(furi_hal_shared_pins_acquire(&wardriver_gps_hw)) return true;
        furi_delay_ms(WARDRIVER_GPS_PINS_TRY_MS);
    }
    return false;
}

static WardriverGpsUartResult wardriver_gps_uart_open(
    int uart,
    int rx,
    int tx,
    const uart_config_t* config) {
    if(wardriver_gps_hw.driver || wardriver_gps_hw.uart_leased ||
       wardriver_gps_hw.pins_leased || wardriver_gps_hw.pins_saved ||
       wardriver_gps_hw.sleep_inhibited) {
        /* An earlier stop could not finish. Never start over on top of it. */
        if(!wardriver_gps_uart_close()) return WardriverGpsUartCleanup;
    }
    wardriver_gps_hw.uart = uart;
    wardriver_gps_hw.rx = rx;
    wardriver_gps_hw.tx = tx;

    const FuriHalUartSharedAcquireResult lease =
        furi_hal_uart_shared_acquire(WARDRIVER_GPS_UART_LEASE_MS);
    if(lease != FuriHalUartSharedAcquireAcquired) {
        return lease == FuriHalUartSharedAcquireBusy ? WardriverGpsUartBusy :
                                                       WardriverGpsUartError;
    }
    wardriver_gps_hw.uart_leased = true;

    WardriverGpsUartResult result = WardriverGpsUartBusy;
    const bool shared = wardriver_gps_pin_is_shared(rx) || wardriver_gps_pin_is_shared(tx);
    if(shared) {
        result = WardriverGpsUartPinsBusy;
        if(!wardriver_gps_acquire_pins()) goto fail;
        wardriver_gps_hw.pins_leased = true;
    }

    /* UART1 (RFID, USB-UART) and UART2 (BW16) both route to GPIO43/44, so a driver
     * on either one conflicts with those pins whichever port GPS asked for. */
    result = WardriverGpsUartBusy;
    if(uart_is_driver_installed(uart)) goto fail;
    if(shared && (uart_is_driver_installed(UART_NUM_1) || uart_is_driver_installed(UART_NUM_2))) {
        goto fail;
    }

    /* False means this board has no GPIO43/44 mux state to snapshot. */
    if(shared && furi_hal_shared_pins_save(&wardriver_gps_hw)) {
        wardriver_gps_hw.pins_saved = true;
    }

    /* Only a start that will really install pays for the veto; refusals above
     * never touch the light-sleep gate. */
    furi_hal_power_light_sleep_inhibit_enter();
    wardriver_gps_hw.sleep_inhibited = true;

    result = WardriverGpsUartError;
    if(uart_driver_install(uart, 2048, 0, 0, NULL, 0) != ESP_OK) goto fail;
    wardriver_gps_hw.driver = true;
    if(uart_param_config(uart, config) != ESP_OK ||
       uart_set_pin(uart, tx, rx, -1, -1) != ESP_OK) {
        goto fail;
    }
    return WardriverGpsUartOk;

fail:
    return wardriver_gps_uart_close() ? result : WardriverGpsUartCleanup;
}

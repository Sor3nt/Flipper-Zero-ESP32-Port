#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Stop btshim, init BLE controller + Bluedroid for raw advertising.
 *  @return true on success */
bool ble_spam_hal_start(void);

/** Stop BLE spam advertising, deinit stack, restart btshim. */
void ble_spam_hal_stop(void);

/** Set raw advertising data and start advertising.
 *  @param data   complete raw AD structure (max 31 bytes)
 *  @param len    data length
 *  @return true if data was set successfully */
bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len);

/** Stop current advertising (call between payload switches). */
void ble_spam_hal_stop_adv(void);

/** Set a new random static BLE address. Call while advertising is stopped. */
void ble_spam_hal_set_random_addr(void);

/** Set a specific BLE address for cloning. Call while advertising is stopped. */
void ble_spam_hal_set_addr(const uint8_t addr[6]);

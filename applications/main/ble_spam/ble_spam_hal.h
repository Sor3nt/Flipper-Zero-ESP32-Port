#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Stop btshim, init BLE controller + Bluedroid for raw advertising.
 *  @return true on success */
bool ble_spam_hal_start(void);

/** Stop BLE spam advertising, deinit stack, restart btshim. */
void ble_spam_hal_stop(void);

/** Set raw advertising data and wait for its completion.
 *  @param data   complete raw AD structure (max 31 bytes)
 *  @param len    data length
 *  @return true if data was accepted by the controller */
bool ble_spam_hal_set_adv_data(const uint8_t* data, uint8_t len);

/** Set the legacy advertising interval from the payload dwell delay (20..500ms). */
bool ble_spam_hal_set_adv_interval_ms(uint32_t delay_ms);

/** Start advertising the most recently configured payload. */
bool ble_spam_hal_start_adv(void);

/** Stop current advertising and wait for the stop completion event. */
bool ble_spam_hal_stop_adv(void);

/** Set a new random static BLE address and wait for completion. */
bool ble_spam_hal_set_random_addr(void);

/** Set a specific BLE address for cloning. Call while advertising is stopped. */
void ble_spam_hal_set_addr(const uint8_t addr[6]);

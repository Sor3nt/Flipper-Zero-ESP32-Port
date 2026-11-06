/**
 * @file furi_hal_reboot_combo.h
 * Forced reboot: hold the board's two reboot buttons (BOARD_REBOOT_COMBO_PIN_A
 * and _B, both active low) for 5 s.
 *
 * Built only with CONFIG_FURI_HAL_REBOOT_COMBO (dual-core boards that opt in
 * through their sdkconfig.defaults.<board>). The check runs from an
 * ISR-dispatched esp_timer on CPU1 and reads the GPIO input registers
 * directly, so it works even when CPU0 or the input service is stuck.
 *
 * The reset is issued as soon as the hold duration is reached. The ESP32-S3
 * software reset does not re-latch the strapping pins (ESP32-S3 datasheet §3,
 * they are latched at Chip Reset only).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the combo timer. Safe to call more than once. No-op when disabled. */
void furi_hal_reboot_combo_init(void);

/** True while both combo buttons are held (input drivers mute repeats then). */
bool furi_hal_reboot_combo_is_held(void);

#ifdef __cplusplus
}
#endif

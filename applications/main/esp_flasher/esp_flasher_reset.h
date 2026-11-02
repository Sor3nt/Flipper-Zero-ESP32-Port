#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How the target's EN (reset) and IO0 (boot strap) lines are driven. */
typedef enum {
    /* Lines untouched: hold BOOT and tap RESET on the target by hand. */
    EspFlasherResetModeOff,
    /* Active-low, idle high: the levels of a USB-UART's RTS/DTR. Works for a classic
     * esptool auto-reset transistor pair and for EN/IO0 wired directly. */
    EspFlasherResetModeEsptool,
    /* Active-high, idle low: Flipper-style boards whose transistors are driven by the
     * Flipper's GPIO (what the original app drives on pins 6/7/15/16). */
    EspFlasherResetModeFlipper,
    EspFlasherResetModeCount,
} EspFlasherResetMode;

bool esp_flasher_reset_available(void);

/* Drives both lines to their released level for mode (Off releases the pins). Returns
 * false when a non-Off mode could not be applied because a pin is claimed by another
 * feature (e.g. an attached external Sub-GHz/NRF24 module) -- the lines are then left
 * undriven (as if Off), and the caller should not treat the mode as active. */
bool esp_flasher_reset_lines_init(EspFlasherResetMode mode);
/* Returns both pins to inputs. */
void esp_flasher_reset_lines_deinit(void);

/* Return false when the lines are not driven (mode Off or no pins on this board). */
bool esp_flasher_reset_target(EspFlasherResetMode mode);
bool esp_flasher_reset_enter_bootloader(EspFlasherResetMode mode);


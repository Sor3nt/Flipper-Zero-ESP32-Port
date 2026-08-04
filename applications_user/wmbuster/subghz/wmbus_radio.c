#include "wmbus_radio.h"
/* Internal on-board CC1101 only.
 *
 * The external (GPIO-attached) CC1101 support was removed: on the LilyGo
 * T-Embed the CC1101 shares the SPI2 bus with the LCD (MOSI=9, SCLK=11), and
 * the "external" SPI handle is a bit-banged bus on those same pins. Driving it
 * toggles the LCD's MOSI/SCLK lines directly and freezes the display. The
 * on-board CC1101 is driven over hardware SPI2 via furi_hal_subghz (see
 * wmbus_worker.c), exactly like the built-in SubGHz app, and coexists with the
 * LCD through the shared-bus mutex.
 *
 * These stubs remain so the rest of the app keeps compiling; nothing selects
 * an external device anymore. */

#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <furi.h>

#define TAG "WmbusRadio"

bool wmbus_radio_detect_external(void) {
    return false;
}

const SubGhzDevice* wmbus_radio_select(const SubGhzDevice* current, WmbusModuleSetting module) {
    (void)current;
    (void)module;
    return subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
}

bool wmbus_radio_is_external(const SubGhzDevice* dev) {
    (void)dev;
    return false;
}

void wmbus_radio_release(const SubGhzDevice* dev) {
    (void)dev;
}

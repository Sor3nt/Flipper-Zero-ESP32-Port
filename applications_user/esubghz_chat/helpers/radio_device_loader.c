/*
 * Ported from https://github.com/twisted-pear/esubghz_chat (GPL-3.0),
 * adapted to match the working pattern already used by this port's own
 * protopirate FAP (applications_user/protopirate/helpers/radio_device_loader.c)
 * for the same int/ext CC1101 selection on this exact codebase, instead of
 * upstream's own version (which #included Flipper-official's
 * cc1101_int/ext_interconnect.h paths directly).
 */
#include "radio_device_loader.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "RadioDeviceLoader"

static bool radio_device_loader_otg_enabled_by_loader = false;

static void radio_device_loader_power_on(void) {
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        /* CC1101 power-up time */
        furi_delay_ms(10);
    }
    if(furi_hal_power_is_otg_enabled()) {
        radio_device_loader_otg_enabled_by_loader = true;
    }
    FURI_LOG_D(TAG, "OTG power enabled after %d attempts", attempts);
}

static void radio_device_loader_power_off(void) {
    if(radio_device_loader_otg_enabled_by_loader && furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
        radio_device_loader_otg_enabled_by_loader = false;
        FURI_LOG_D(TAG, "OTG power disabled");
    }
}

bool radio_device_loader_is_connect_external(const char* name) {
    bool is_connect = false;
    bool is_otg_enabled = furi_hal_power_is_otg_enabled();

    if(!is_otg_enabled) {
        radio_device_loader_power_on();
    }

    const SubGhzDevice* device = subghz_devices_get_by_name(name);
    if(device) {
        is_connect = subghz_devices_is_connect(device);
        FURI_LOG_D(TAG, "External device '%s' connect check: %s", name, is_connect ? "YES" : "NO");
    } else {
        FURI_LOG_W(TAG, "Could not get device by name: %s", name);
    }

    if(!is_otg_enabled) {
        radio_device_loader_power_off();
    }
    return is_connect;
}

const SubGhzDevice* radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzRadioDeviceType radio_device_type) {
    const SubGhzDevice* radio_device;

    if(radio_device_type == SubGhzRadioDeviceTypeExternalCC1101 &&
       radio_device_loader_is_connect_external(SUBGHZ_DEVICE_CC1101_EXT_NAME)) {
        radio_device_loader_power_on();
        radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
        subghz_devices_begin(radio_device);
    } else if(current_radio_device == NULL) {
        radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        subghz_devices_begin(radio_device);
    } else {
        radio_device_loader_end(current_radio_device);
        radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        subghz_devices_begin(radio_device);
    }

    return radio_device;
}

void radio_device_loader_end(const SubGhzDevice* radio_device) {
    furi_assert(radio_device);
    /* Matches protopirate's radio_device_loader_end() on this same port:
     * the internal CC1101 device is left running (subghz_devices_end() on
     * it is unnecessary and, on this port's device implementation, not
     * something other code expects) -- only an external device is
     * actually ended. */
    if(radio_device != subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME)) {
        subghz_devices_end(radio_device);
    }
    radio_device_loader_power_off();
}

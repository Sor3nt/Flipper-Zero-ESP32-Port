/*
 * Ported from https://github.com/twisted-pear/esubghz_chat (GPL-3.0),
 * adapted to match the working pattern already used by this port's own
 * protopirate FAP (applications_user/protopirate/helpers/radio_device_loader.h)
 * for the same int/ext CC1101 selection on this exact codebase.
 */
#pragma once

#include <lib/subghz/devices/devices.h>

#define SUBGHZ_DEVICE_CC1101_INT_NAME "cc1101_int"
#define SUBGHZ_DEVICE_CC1101_EXT_NAME "cc1101_ext"

/** SubGhzRadioDeviceType */
typedef enum {
    SubGhzRadioDeviceTypeInternal,
    SubGhzRadioDeviceTypeExternalCC1101,
} SubGhzRadioDeviceType;

const SubGhzDevice* radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzRadioDeviceType radio_device_type);

bool radio_device_loader_is_connect_external(const char* name);
void radio_device_loader_end(const SubGhzDevice* radio_device);

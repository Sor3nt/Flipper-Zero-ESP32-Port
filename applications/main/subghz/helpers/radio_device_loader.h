#pragma once

#include <lib/subghz/devices/devices.h>

#define SUBGHZ_DEVICE_CC1101_INT_NAME "cc1101_int"
#define SUBGHZ_DEVICE_CC1101_EXT_NAME "cc1101_ext"

typedef enum {
    SubGhzJammerRadioDeviceTypeInternal,
    SubGhzJammerRadioDeviceTypeExternalCC1101,
} SubGhzJammerRadioDeviceType;

const SubGhzDevice* jammer_radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzJammerRadioDeviceType radio_device_type);

bool jammer_radio_device_loader_is_connect_external(const char* name);
bool jammer_radio_device_loader_is_external(const SubGhzDevice* radio_device);
void jammer_radio_device_loader_end(const SubGhzDevice* radio_device);

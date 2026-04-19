#include "registry.h"

#include "cc1101_int/cc1101_int_interconnect.h"

#define TAG "SubGhzDeviceRegistry"

struct SubGhzDeviceRegistry {
    const SubGhzDevice** items;
    size_t size;
};

static SubGhzDeviceRegistry* subghz_device_registry = NULL;
extern const SubGhzDevice subghz_device_cc1101_ext;

void subghz_device_registry_init(void) {
    SubGhzDeviceRegistry* subghz_device =
        (SubGhzDeviceRegistry*)malloc(sizeof(SubGhzDeviceRegistry));
    subghz_device->size = 2;
    subghz_device->items =
        (const SubGhzDevice**)malloc(sizeof(SubGhzDevice*) * subghz_device->size);
    subghz_device->items[0] = &subghz_device_cc1101_int;
    subghz_device->items[1] = &subghz_device_cc1101_ext;

    FURI_LOG_I(TAG, "Loaded %zu radio device", subghz_device->size);
    subghz_device_registry = subghz_device;
}

void subghz_device_registry_deinit(void) {
    free(subghz_device_registry->items);
    free(subghz_device_registry);
    subghz_device_registry = NULL;
}

bool subghz_device_registry_is_valid(void) {
    return subghz_device_registry != NULL;
}

const SubGhzDevice* subghz_device_registry_get_by_name(const char* name) {
    furi_assert(subghz_device_registry);

    if(name != NULL) {
        for(size_t i = 0; i < subghz_device_registry->size; i++) {
            if(strcmp(name, subghz_device_registry->items[i]->name) == 0) {
                return subghz_device_registry->items[i];
            }
        }
    }
    return NULL;
}

const SubGhzDevice* subghz_device_registry_get_by_index(size_t index) {
    furi_assert(subghz_device_registry);
    if(index < subghz_device_registry->size) {
        return subghz_device_registry->items[index];
    } else {
        return NULL;
    }
}

#include <stdint.h>
#include <string.h>
#include <furi.h>

#include "animation_manager.h"
#include "animation_storage.h"
#include "animation_storage_i.h"
#include "assets_dolphin_internal.h"
#include "assets_dolphin_blocking.h"

#define TAG "AnimationStorage"

StorageAnimation* animation_storage_find_animation(const char* name) {
    furi_assert(name);
    furi_assert(strlen(name));

    for(size_t i = 0; i < dolphin_blocking_size; ++i) {
        if(!strcmp(dolphin_blocking[i].manifest_info.name, name)) {
            return (StorageAnimation*)&dolphin_blocking[i];
        }
    }

    for(size_t i = 0; i < dolphin_internal_size; ++i) {
        if(!strcmp(dolphin_internal[i].manifest_info.name, name)) {
            return (StorageAnimation*)&dolphin_internal[i];
        }
    }

    FURI_LOG_E(TAG, "Animation '%s' not found", name);
    return NULL;
}

StorageAnimationManifestInfo* animation_storage_get_meta(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    return &storage_animation->manifest_info;
}

const BubbleAnimation*
    animation_storage_get_bubble_animation(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    return storage_animation->animation;
}

void animation_storage_free_storage_animation(StorageAnimation** storage_animation) {
    furi_assert(storage_animation);
    furi_assert(*storage_animation);

    // Internal animations are const, don't free them
    // Just null the pointer
    *storage_animation = NULL;
}

size_t animation_storage_get_internal_list(const StorageAnimation** list) {
    *list = dolphin_internal;
    return dolphin_internal_size;
}

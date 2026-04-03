#include <view_stack.h>
#include <stdint.h>
#include <string.h>
#include <furi.h>
#include <furi_hal.h>

#include "views/bubble_animation_view.h"
#include "views/one_shot_animation_view.h"
#include "animation_storage.h"
#include "animation_manager.h"
#include "assets_dolphin_internal.h"

#include <furi_hal_random.h>

#define TAG "AnimationManager"

#define HARDCODED_ANIMATION_NAME "L1_Tv_128x47"

typedef enum {
    AnimationManagerStateIdle,
    AnimationManagerStateFreezedIdle,
} AnimationManagerState;

struct AnimationManager {
    AnimationManagerState state;
    BubbleAnimationView* animation_view;
    FuriTimer* idle_animation_timer;
    StorageAnimation* current_animation;
    AnimationManagerInteractCallback interact_callback;
    AnimationManagerSetNewIdleAnimationCallback new_idle_callback;
    AnimationManagerSetNewIdleAnimationCallback check_blocking_callback;
    void* context;
    FuriString* freezed_animation_name;
    int32_t freezed_animation_time_left;
    ViewStack* view_stack;
};

static StorageAnimation*
    animation_manager_select_idle_animation(AnimationManager* animation_manager);
static void animation_manager_replace_current_animation(
    AnimationManager* animation_manager,
    StorageAnimation* storage_animation);
static void animation_manager_start_new_idle(AnimationManager* animation_manager);

void animation_manager_set_context(AnimationManager* animation_manager, void* context) {
    furi_assert(animation_manager);
    animation_manager->context = context;
}

void animation_manager_set_new_idle_callback(
    AnimationManager* animation_manager,
    AnimationManagerSetNewIdleAnimationCallback callback) {
    furi_assert(animation_manager);
    animation_manager->new_idle_callback = callback;
}

void animation_manager_set_check_callback(
    AnimationManager* animation_manager,
    AnimationManagerCheckBlockingCallback callback) {
    furi_assert(animation_manager);
    animation_manager->check_blocking_callback = callback;
}

void animation_manager_set_interact_callback(
    AnimationManager* animation_manager,
    AnimationManagerInteractCallback callback) {
    furi_assert(animation_manager);
    animation_manager->interact_callback = callback;
}

void animation_manager_set_dummy_mode_state(AnimationManager* animation_manager, bool enabled) {
    furi_assert(animation_manager);
    (void)enabled;
}

static void animation_manager_timer_callback(void* context) {
    furi_assert(context);
    AnimationManager* animation_manager = context;
    if(animation_manager->new_idle_callback) {
        animation_manager->new_idle_callback(animation_manager->context);
    }
}

static void animation_manager_interact_callback(void* context) {
    furi_assert(context);
    AnimationManager* animation_manager = context;
    if(animation_manager->interact_callback) {
        animation_manager->interact_callback(animation_manager->context);
    }
}

void animation_manager_check_blocking_process(AnimationManager* animation_manager) {
    furi_assert(animation_manager);
    // No blocking animations on ESP32 (no SD card checks, no dolphin level)
}

void animation_manager_new_idle_process(AnimationManager* animation_manager) {
    furi_assert(animation_manager);

    if(animation_manager->state == AnimationManagerStateIdle) {
        animation_manager_start_new_idle(animation_manager);
    }
}

bool animation_manager_interact_process(AnimationManager* animation_manager) {
    furi_assert(animation_manager);
    // No blocking/levelup on ESP32
    return false;
}

static void animation_manager_start_new_idle(AnimationManager* animation_manager) {
    furi_assert(animation_manager);

    FURI_LOG_I(TAG, "Selecting idle animation...");
    StorageAnimation* new_animation = animation_manager_select_idle_animation(animation_manager);
    FURI_LOG_I(TAG, "Selected animation: %p", new_animation);

    animation_manager_replace_current_animation(animation_manager, new_animation);
    const BubbleAnimation* bubble_animation =
        animation_storage_get_bubble_animation(animation_manager->current_animation);
    FURI_LOG_I(
        TAG,
        "Animation: frames=%u+%u, duration=%u, frame_rate=%u, w=%u, h=%u",
        bubble_animation->passive_frames,
        bubble_animation->active_frames,
        bubble_animation->duration,
        bubble_animation->icon_animation.frame_rate,
        bubble_animation->icon_animation.width,
        bubble_animation->icon_animation.height);
    animation_manager->state = AnimationManagerStateIdle;
    furi_timer_start(animation_manager->idle_animation_timer, bubble_animation->duration * 1000);
}

static void animation_manager_replace_current_animation(
    AnimationManager* animation_manager,
    StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    StorageAnimation* previous_animation = animation_manager->current_animation;

    const BubbleAnimation* animation = animation_storage_get_bubble_animation(storage_animation);
    bubble_animation_view_set_animation(animation_manager->animation_view, animation);
    const char* new_name = animation_storage_get_meta(storage_animation)->name;
    FURI_LOG_I(TAG, "Select '%s' animation", new_name);
    animation_manager->current_animation = storage_animation;

    if(previous_animation) {
        animation_storage_free_storage_animation(&previous_animation);
    }
}

AnimationManager* animation_manager_alloc(void) {
    FURI_LOG_I(TAG, "Alloc start");
    AnimationManager* animation_manager = malloc(sizeof(AnimationManager));
    memset(animation_manager, 0, sizeof(AnimationManager));

    FURI_LOG_I(TAG, "Creating bubble animation view...");
    animation_manager->animation_view = bubble_animation_view_alloc();
    FURI_LOG_I(TAG, "Bubble animation view: %p", animation_manager->animation_view);

    animation_manager->view_stack = view_stack_alloc();
    View* animation_view = bubble_animation_get_view(animation_manager->animation_view);
    FURI_LOG_I(TAG, "View from bubble: %p", animation_view);
    view_stack_add_view(animation_manager->view_stack, animation_view);
    animation_manager->freezed_animation_name = furi_string_alloc();

    animation_manager->idle_animation_timer =
        furi_timer_alloc(animation_manager_timer_callback, FuriTimerTypeOnce, animation_manager);
    bubble_animation_view_set_interact_callback(
        animation_manager->animation_view,
        animation_manager_interact_callback,
        animation_manager);

    FURI_LOG_I(TAG, "Starting first idle animation...");
    animation_manager_start_new_idle(animation_manager);
    FURI_LOG_I(TAG, "Alloc complete");

    return animation_manager;
}

void animation_manager_free(AnimationManager* animation_manager) {
    furi_assert(animation_manager);

    furi_string_free(animation_manager->freezed_animation_name);
    View* animation_view = bubble_animation_get_view(animation_manager->animation_view);
    view_stack_remove_view(animation_manager->view_stack, animation_view);
    bubble_animation_view_free(animation_manager->animation_view);
    furi_timer_free(animation_manager->idle_animation_timer);
    free(animation_manager);
}

View* animation_manager_get_animation_view(AnimationManager* animation_manager) {
    furi_assert(animation_manager);

    return view_stack_get_view(animation_manager->view_stack);
}

static bool animation_manager_is_valid_idle_animation(const StorageAnimation* anim) {
    const char* name = anim->manifest_info.name;

    /* Skip NoSd — always assume SD present (no blocking Storage check) */
    if(strcmp(name, "L1_NoSd_128x49") == 0) return false;

    /* BadBattery — skip on ESP32, no battery monitoring */
    if(strcmp(name, "L1_BadBattery_128x47") == 0) return false;

    return true;
}

static StorageAnimation*
    animation_manager_select_idle_animation(AnimationManager* animation_manager) {
    (void)animation_manager;

    /* Pick a random internal animation based on weight, filtering by validity */
    const StorageAnimation* list;
    size_t count = animation_storage_get_internal_list(&list);
    FURI_LOG_I(TAG, "Internal animation count: %u", count);

    uint32_t whole_weight = 0;
    for(size_t i = 0; i < count; ++i) {
        if(animation_manager_is_valid_idle_animation(&list[i])) {
            whole_weight += list[i].manifest_info.weight;
        }
    }

    if(whole_weight == 0) {
        /* Fallback to hardcoded */
        return animation_storage_find_animation(HARDCODED_ANIMATION_NAME);
    }

    uint32_t lucky_number = furi_hal_random_get() % whole_weight;
    uint32_t weight = 0;
    StorageAnimation* selected = NULL;

    for(size_t i = 0; i < count; ++i) {
        if(!animation_manager_is_valid_idle_animation(&list[i])) continue;
        weight += list[i].manifest_info.weight;
        if(lucky_number < weight) {
            selected = (StorageAnimation*)&list[i];
            break;
        }
    }

    if(!selected) {
        selected = animation_storage_find_animation(HARDCODED_ANIMATION_NAME);
    }

    furi_assert(selected);
    return selected;
}

bool animation_manager_is_animation_loaded(AnimationManager* animation_manager) {
    furi_assert(animation_manager);
    return animation_manager->current_animation != NULL;
}

void animation_manager_unload_and_stall_animation(AnimationManager* animation_manager) {
    furi_assert(animation_manager);
    furi_assert(animation_manager->current_animation);
    furi_assert(!furi_string_size(animation_manager->freezed_animation_name));
    furi_assert(animation_manager->state == AnimationManagerStateIdle);

    animation_manager->state = AnimationManagerStateFreezedIdle;

    animation_manager->freezed_animation_time_left =
        furi_timer_get_expire_time(animation_manager->idle_animation_timer) - furi_get_tick();
    if(animation_manager->freezed_animation_time_left < 0) {
        animation_manager->freezed_animation_time_left = 0;
    }
    furi_timer_stop(animation_manager->idle_animation_timer);

    FURI_LOG_I(
        TAG,
        "Unload animation '%s'",
        animation_storage_get_meta(animation_manager->current_animation)->name);

    StorageAnimationManifestInfo* meta =
        animation_storage_get_meta(animation_manager->current_animation);
    furi_string_set(animation_manager->freezed_animation_name, meta->name);

    bubble_animation_freeze(animation_manager->animation_view);
    animation_storage_free_storage_animation(&animation_manager->current_animation);
}

void animation_manager_load_and_continue_animation(AnimationManager* animation_manager) {
    furi_assert(animation_manager);
    furi_assert(!animation_manager->current_animation);
    furi_assert(furi_string_size(animation_manager->freezed_animation_name));
    furi_assert(animation_manager->state == AnimationManagerStateFreezedIdle);

    StorageAnimation* restore_animation = animation_storage_find_animation(
        furi_string_get_cstr(animation_manager->freezed_animation_name));
    if(restore_animation) {
        animation_manager_replace_current_animation(animation_manager, restore_animation);
        animation_manager->state = AnimationManagerStateIdle;

        if(animation_manager->freezed_animation_time_left) {
            furi_timer_start(
                animation_manager->idle_animation_timer,
                animation_manager->freezed_animation_time_left);
        } else {
            const BubbleAnimation* animation =
                animation_storage_get_bubble_animation(animation_manager->current_animation);
            furi_timer_start(
                animation_manager->idle_animation_timer, animation->duration * 1000);
        }
    }

    if(!animation_manager->current_animation) {
        animation_manager_start_new_idle(animation_manager);
    }

    FURI_LOG_I(
        TAG,
        "Load animation '%s'",
        animation_storage_get_meta(animation_manager->current_animation)->name);

    bubble_animation_unfreeze(animation_manager->animation_view);
    furi_string_reset(animation_manager->freezed_animation_name);
    furi_assert(animation_manager->current_animation);
}

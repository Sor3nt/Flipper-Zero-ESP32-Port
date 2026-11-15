
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <core/dangerous_defines.h>
#include <storage/storage.h>
#include <gui/icon_i.h>
#include <momentum_settings.h>
#include <sdkconfig.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include "animation_manager.h"
#include "animation_storage.h"
#include <assets_dolphin_internal.h>
#include <assets_dolphin_blocking.h>

#define ANIMATION_META_FILE     "meta.txt"
#define BASE_ANIMATION_DIR      EXT_PATH("dolphin")
#define ANIMATION_DIR_MAX       64
#define ANIMATION_MANIFEST_MAX  80
#define TAG                     "AnimationStorage"

static char animation_dir[ANIMATION_DIR_MAX];
static char animation_manifest_file[ANIMATION_MANIFEST_MAX];

static void animation_storage_free_bubbles(BubbleAnimation* animation);
static void animation_storage_free_frames(BubbleAnimation* animation);
static void animation_storage_free_animation(BubbleAnimation** storage_animation);
static BubbleAnimation* animation_storage_load_animation(const char* name);

static bool animation_storage_manifest_at(Storage* storage, const char* dir) {
    FuriString* path = furi_string_alloc_printf("%s/manifest.txt", dir);
    FileInfo info;
    bool exists = storage_common_stat(storage, furi_string_get_cstr(path), &info) == FSE_OK;
    furi_string_free(path);
    return exists;
}

/* Built-in idle animation that is always valid; the other built-in ones only
 * apply in their own state (bad battery, no SD card). */
#define ANIMATION_STORAGE_BUILTIN_IDLE_NAME "L1_Tv_128x47"

/* Selects the manifest for the current asset pack; returns true if it is the
 * pack's own manifest (false: default /ext/dolphin). */
static bool animation_storage_select_manifest(void) {
    FuriString* path = furi_string_alloc();
    char pack[ASSET_PACKS_NAME_LEN];
    momentum_settings_get_asset_pack(pack, sizeof(pack));
    bool use_asset_pack = pack[0] != '\0';

    if(use_asset_pack) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        furi_string_printf(path, "%s/%s/Anims", ASSET_PACKS_PATH, pack);
        if(!animation_storage_manifest_at(storage, furi_string_get_cstr(path))) {
            furi_string_printf(path, "%s/%s", ASSET_PACKS_PATH, pack);
            if(!animation_storage_manifest_at(storage, furi_string_get_cstr(path))) {
                FURI_LOG_W(TAG, "Asset pack '%s' has no manifest; using default animations", pack);
                use_asset_pack = false;
            }
        }
        furi_record_close(RECORD_STORAGE);
    }

    if(!use_asset_pack) {
        furi_string_set(path, BASE_ANIMATION_DIR);
    }
    snprintf(animation_dir, sizeof(animation_dir), "%s", furi_string_get_cstr(path));
    snprintf(
        animation_manifest_file,
        sizeof(animation_manifest_file),
        "%s/manifest.txt",
        animation_dir);
    furi_string_free(path);
    return use_asset_pack;
}

static bool animation_storage_load_single_manifest_info(
    StorageAnimationManifestInfo* manifest_info,
    const char* name) {
    furi_assert(manifest_info);
    animation_storage_select_manifest();

    bool result = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    flipper_format_set_strict_mode(file, true);
    FuriString* read_string;
    read_string = furi_string_alloc();

    do {
        uint32_t u32value;
        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_existing(file, animation_manifest_file)) break;

        if(!flipper_format_read_header(file, read_string, &u32value)) break;
        if(furi_string_cmp_str(read_string, "Flipper Animation Manifest")) break;

        manifest_info->name = NULL;

        /* skip other animation names */
        flipper_format_set_strict_mode(file, false);
        while(flipper_format_read_string(file, "Name", read_string) &&
              furi_string_cmp_str(read_string, name))
            ;
        if(furi_string_cmp_str(read_string, name)) break;
        flipper_format_set_strict_mode(file, true);

        manifest_info->name = strdup(furi_string_get_cstr(read_string));

        if(!flipper_format_read_uint32(file, "Min butthurt", &u32value, 1)) break;
        manifest_info->min_butthurt = u32value;
        if(!flipper_format_read_uint32(file, "Max butthurt", &u32value, 1)) break;
        manifest_info->max_butthurt = u32value;
        if(!flipper_format_read_uint32(file, "Min level", &u32value, 1)) break;
        manifest_info->min_level = u32value;
        if(!flipper_format_read_uint32(file, "Max level", &u32value, 1)) break;
        manifest_info->max_level = u32value;
        if(!flipper_format_read_uint32(file, "Weight", &u32value, 1)) break;
        manifest_info->weight = u32value;
        result = true;
    } while(0);

    if(!result && manifest_info->name) {
        free((void*)manifest_info->name);
    }
    furi_string_free(read_string);
    flipper_format_free(file);

    furi_record_close(RECORD_STORAGE);

    return result;
}

void animation_storage_fill_animation_list(StorageAnimationList_t* animation_list) {
    furi_assert(sizeof(StorageAnimationList_t) == sizeof(void*));
    furi_assert(!StorageAnimationList_size(*animation_list));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    const bool from_asset_pack = animation_storage_select_manifest();
    size_t manifest_entries = 0;
    FlipperFormat* file = flipper_format_file_alloc(storage);
    /* Forbid skipping fields */
    flipper_format_set_strict_mode(file, true);
    FuriString* read_string;
    read_string = furi_string_alloc();

    do {
        uint32_t u32value;
        StorageAnimation* storage_animation = NULL;

        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_existing(file, animation_manifest_file)) break;
        if(!flipper_format_read_header(file, read_string, &u32value)) break;
        if(furi_string_cmp_str(read_string, "Flipper Animation Manifest")) break;
        do {
            storage_animation = malloc(sizeof(StorageAnimation));
            storage_animation->external = true;
            storage_animation->animation = NULL;
            storage_animation->manifest_info.name = NULL;

            if(!flipper_format_read_string(file, "Name", read_string)) break;
            storage_animation->manifest_info.name = strdup(furi_string_get_cstr(read_string));

            if(!flipper_format_read_uint32(file, "Min butthurt", &u32value, 1)) break;
            storage_animation->manifest_info.min_butthurt = u32value;
            if(!flipper_format_read_uint32(file, "Max butthurt", &u32value, 1)) break;
            storage_animation->manifest_info.max_butthurt = u32value;
            if(!flipper_format_read_uint32(file, "Min level", &u32value, 1)) break;
            storage_animation->manifest_info.min_level = u32value;
            if(!flipper_format_read_uint32(file, "Max level", &u32value, 1)) break;
            storage_animation->manifest_info.max_level = u32value;
            if(!flipper_format_read_uint32(file, "Weight", &u32value, 1)) break;
            storage_animation->manifest_info.weight = u32value;

            StorageAnimationList_push_back(*animation_list, storage_animation);
            manifest_entries++;
        } while(1);

        animation_storage_free_storage_animation(&storage_animation);
    } while(0);

    furi_string_free(read_string);
    flipper_format_free(file);

    /* add hard-coded animations. With an asset pack that lists animations, the
     * always-valid built-in idle one stays out of the draw: it would replace
     * the pack's animation at random (1 in 2 for a one-animation pack). It is
     * still the fallback when none of the pack's animations is valid. */
    const bool skip_builtin_idle = from_asset_pack && manifest_entries > 0;
    for(size_t i = 0; i < dolphin_internal_size; ++i) {
        if(skip_builtin_idle &&
           !strcmp(dolphin_internal[i].manifest_info.name, ANIMATION_STORAGE_BUILTIN_IDLE_NAME)) {
            continue;
        }
        StorageAnimationList_push_back(*animation_list, (StorageAnimation*)&dolphin_internal[i]);
    }

    furi_record_close(RECORD_STORAGE);
}

StorageAnimation* animation_storage_find_animation(const char* name) {
    furi_assert(name);
    furi_assert(strlen(name));
    StorageAnimation* storage_animation = NULL;

    for(size_t i = 0; i < dolphin_blocking_size; ++i) {
        if(!strcmp(dolphin_blocking[i].manifest_info.name, name)) {
            storage_animation = (StorageAnimation*)&dolphin_blocking[i];
            break;
        }
    }

    if(!storage_animation) {
        for(size_t i = 0; i < dolphin_internal_size; ++i) {
            if(!strcmp(dolphin_internal[i].manifest_info.name, name)) {
                storage_animation = (StorageAnimation*)&dolphin_internal[i];
                break;
            }
        }
    }

    /* look through external animations */
    if(!storage_animation) {
        storage_animation = malloc(sizeof(StorageAnimation));
        storage_animation->external = true;

        bool result = false;
        result =
            animation_storage_load_single_manifest_info(&storage_animation->manifest_info, name);
        if(result) {
            storage_animation->animation = animation_storage_load_animation(name);
            result = !!storage_animation->animation;
        }
        if(!result) {
            animation_storage_free_storage_animation(&storage_animation);
        }
    }

    return storage_animation;
}

StorageAnimationManifestInfo* animation_storage_get_meta(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    return &storage_animation->manifest_info;
}

const BubbleAnimation*
    animation_storage_get_bubble_animation(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    animation_storage_cache_animation(storage_animation);
    return storage_animation->animation;
}

void animation_storage_cache_animation(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);

    if(storage_animation->external) {
        if(!storage_animation->animation) {
            storage_animation->animation =
                animation_storage_load_animation(storage_animation->manifest_info.name);
        }
    }
}

static void animation_storage_free_animation(BubbleAnimation** animation) {
    furi_assert(animation);

    if(*animation) {
        animation_storage_free_bubbles(*animation);
        animation_storage_free_frames(*animation);
        if((*animation)->frame_order) {
            free((void*)(*animation)->frame_order);
        }
        free(*animation);
        *animation = NULL;
    }
}

void animation_storage_free_storage_animation(StorageAnimation** storage_animation) {
    furi_assert(storage_animation);
    furi_assert(*storage_animation);

    if((*storage_animation)->external) {
        animation_storage_free_animation((BubbleAnimation**)&(*storage_animation)->animation);

        if((*storage_animation)->manifest_info.name) {
            free((void*)(*storage_animation)->manifest_info.name);
        }
        free(*storage_animation);
    }

    *storage_animation = NULL;
}

static bool animation_storage_cast_align(FuriString* align_str, Align* align) {
    if(!furi_string_cmp(align_str, "Bottom")) {
        *align = AlignBottom;
    } else if(!furi_string_cmp(align_str, "Top")) {
        *align = AlignTop;
    } else if(!furi_string_cmp(align_str, "Left")) {
        *align = AlignLeft;
    } else if(!furi_string_cmp(align_str, "Right")) {
        *align = AlignRight;
    } else if(!furi_string_cmp(align_str, "Center")) {
        *align = AlignCenter;
    } else {
        return false;
    }

    return true;
}

static void animation_storage_free_frames(BubbleAnimation* animation) {
    furi_assert(animation);

    Icon* icon = (Icon*)&animation->icon_animation;
    /* Idempotent: frames may never have been allocated, or may already have
     * been released by a failed animation_storage_load_frames(). */
    if(!icon->frames) return;

    for(int i = 0; i < icon->frame_count; ++i) {
        if(icon->frames[i]) {
            free((void*)icon->frames[i]);
        }
    }

    free((void*)icon->frames);
    icon->frames = NULL;
}

/* Frame data (and the frame pointer table) of SD animations goes to PSRAM when
 * the target has it, so the desktop can keep its animation loaded while an app
 * runs without taking internal RAM from that app (see
 * animation_storage_can_keep_loaded). Zeroed like the furi malloc() it falls
 * back to; freed with free(). */
static void* animation_storage_frame_alloc(size_t size) {
#if CONFIG_SPIRAM
    void* buffer = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(buffer) return buffer;
#endif
    return malloc(size);
}

bool animation_storage_can_keep_loaded(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
#if CONFIG_SPIRAM
    if(!storage_animation->external || !storage_animation->animation) return false;
    const Icon* icon = &storage_animation->animation->icon_animation;
    if(!icon->frames || !esp_ptr_external_ram(icon->frames)) return false;
    for(int i = 0; i < icon->frame_count; ++i) {
        if(!icon->frames[i] || !esp_ptr_external_ram(icon->frames[i])) return false;
    }
    return true;
#else
    UNUSED(storage_animation);
    return false;
#endif
}

static bool animation_storage_load_frames(
    Storage* storage,
    const char* name,
    BubbleAnimation* animation,
    uint32_t* frame_order,
    uint8_t width,
    uint8_t height) {
    uint16_t frame_order_count = animation->passive_frames + animation->active_frames;

    /* The frames should go in order (0...N), without omissions */
    size_t max_frame_count = 0;
    for(int i = 0; i < frame_order_count; ++i) {
        max_frame_count = MAX(max_frame_count, frame_order[i]);
    }

    if((max_frame_count >= frame_order_count) || (max_frame_count >= 256 /* max uint8_t */)) {
        return false;
    }

    Icon* icon = (Icon*)&animation->icon_animation;
    FURI_CONST_ASSIGN(icon->frame_count, max_frame_count + 1);
    FURI_CONST_ASSIGN(icon->frame_rate, 0);
    FURI_CONST_ASSIGN(icon->height, height);
    FURI_CONST_ASSIGN(icon->width, width);
    icon->frames = animation_storage_frame_alloc(sizeof(const uint8_t*) * icon->frame_count);
    if(!icon->frames) return false;

    bool frames_ok = false;
    File* file = storage_file_alloc(storage);
    uint64_t file_size = 0;
    FuriString* filename;
    filename = furi_string_alloc();
    size_t max_filesize = ROUND_UP_TO(width, 8) * height + 1;

    /* One open per frame: the size comes from the open file instead of a
     * separate stat, which walked the whole path on the SD card a second time
     * for every frame (the SD shares its SPI bus with the display). */
    for(int i = 0; i < icon->frame_count; ++i) {
        frames_ok = false;
        file_size = 0;
        furi_string_printf(filename, "%s/%s/frame_%d.bm", animation_dir, name, i);

        if(!storage_file_open(
               file, furi_string_get_cstr(filename), FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_E(TAG, "Can't open file \'%s\'", furi_string_get_cstr(filename));
            break;
        }
        file_size = storage_file_size(file);
        if(file_size == 0 || file_size > max_filesize) {
            FURI_LOG_E(
                TAG,
                "Filesize %llu, max: %zu (width %u, height %u)",
                file_size,
                max_filesize,
                width,
                height);
            storage_file_close(file);
            break;
        }

        FURI_CONST_ASSIGN_PTR(icon->frames[i], animation_storage_frame_alloc(file_size));
        if(!icon->frames[i] ||
           storage_file_read(file, (void*)icon->frames[i], file_size) != file_size) {
            FURI_LOG_E(TAG, "Read failed: \'%s\'", furi_string_get_cstr(filename));
            storage_file_close(file);
            break;
        }
        storage_file_close(file);
        frames_ok = true;
    }

    if(!frames_ok) {
        FURI_LOG_E(
            TAG,
            "Load \'%s\' failed, %ux%u, size: %llu",
            furi_string_get_cstr(filename),
            width,
            height,
            file_size);
        animation_storage_free_frames(animation);
    } else {
        furi_check(animation->icon_animation.frames);
        for(int i = 0; i < animation->icon_animation.frame_count; ++i) {
            furi_check(animation->icon_animation.frames[i]);
        }
    }

    storage_file_free(file);
    furi_string_free(filename);

    return frames_ok;
}

static bool animation_storage_load_bubbles(BubbleAnimation* animation, FlipperFormat* ff) {
    uint32_t u32value;
    FuriString* str;
    str = furi_string_alloc();
    bool success = false;
    furi_assert(!animation->frame_bubble_sequences);

    do {
        if(!flipper_format_read_uint32(ff, "Bubble slots", &u32value, 1)) break;
        if(u32value > 20) break;
        animation->frame_bubble_sequences_count = u32value;
        if(animation->frame_bubble_sequences_count == 0) {
            success = true;
            break;
        }
        animation->frame_bubble_sequences =
            malloc(sizeof(FrameBubble*) * animation->frame_bubble_sequences_count);

        int32_t current_slot = 0;
        for(int i = 0; i < animation->frame_bubble_sequences_count; ++i) {
            FURI_CONST_ASSIGN_PTR(
                animation->frame_bubble_sequences[i], malloc(sizeof(FrameBubble)));
        }

        const FrameBubble* bubble = animation->frame_bubble_sequences[0];
        int8_t index = -1;
        for(;;) {
            if(!flipper_format_read_int32(ff, "Slot", &current_slot, 1)) break;
            if((current_slot != 0) && (index == -1)) break;

            if(current_slot == index) {
                FURI_CONST_ASSIGN_PTR(bubble->next_bubble, malloc(sizeof(FrameBubble)));
                bubble = bubble->next_bubble;
            } else if(current_slot == index + 1) {
                ++index;
                bubble = animation->frame_bubble_sequences[index];
            } else {
                /* slots have to start from 0, be ascending sorted, and
                 * have exact number of slots as specified in "Bubble slots" */
                break;
            }
            if(index >= animation->frame_bubble_sequences_count) break;

            if(!flipper_format_read_uint32(ff, "X", &u32value, 1)) break;
            FURI_CONST_ASSIGN(bubble->bubble.x, u32value);
            if(!flipper_format_read_uint32(ff, "Y", &u32value, 1)) break;
            FURI_CONST_ASSIGN(bubble->bubble.y, u32value);

            if(!flipper_format_read_string(ff, "Text", str)) break;
            if(furi_string_size(str) > 100) break;

            furi_string_replace_all(str, "\\n", "\n");

            FURI_CONST_ASSIGN_PTR(bubble->bubble.text, strdup(furi_string_get_cstr(str)));

            if(!flipper_format_read_string(ff, "AlignH", str)) break;
            if(!animation_storage_cast_align(str, (Align*)&bubble->bubble.align_h)) break;
            if(!flipper_format_read_string(ff, "AlignV", str)) break;
            if(!animation_storage_cast_align(str, (Align*)&bubble->bubble.align_v)) break;

            if(!flipper_format_read_uint32(ff, "StartFrame", &u32value, 1)) break;
            FURI_CONST_ASSIGN(bubble->start_frame, u32value);
            if(!flipper_format_read_uint32(ff, "EndFrame", &u32value, 1)) break;
            FURI_CONST_ASSIGN(bubble->end_frame, u32value);
        }
        success = (index + 1) == animation->frame_bubble_sequences_count;
    } while(0);

    if(!success) {
        if(animation->frame_bubble_sequences) {
            FURI_LOG_E(TAG, "Failed to load animation bubbles");
            animation_storage_free_bubbles(animation);
        }
    }

    furi_string_free(str);
    return success;
}

static BubbleAnimation* animation_storage_load_animation(const char* name) {
    furi_assert(name);
    BubbleAnimation* animation = malloc(sizeof(BubbleAnimation));

    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t* u32array = NULL;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    animation_storage_select_manifest();
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    /* Forbid skipping fields */
    flipper_format_set_strict_mode(ff, true);
    FuriString* str;
    str = furi_string_alloc();
    animation->frame_bubble_sequences = NULL;

    bool success = false;
    do {
        uint32_t u32value;

        if(FSE_OK != storage_sd_status(storage)) break;

        furi_string_printf(str, "%s/%s/" ANIMATION_META_FILE, animation_dir, name);
        if(!flipper_format_file_open_existing(ff, furi_string_get_cstr(str))) break;
        if(!flipper_format_read_header(ff, str, &u32value)) break;
        if(furi_string_cmp_str(str, "Flipper Animation")) break;

        if(!flipper_format_read_uint32(ff, "Width", &width, 1)) break;
        if(!flipper_format_read_uint32(ff, "Height", &height, 1)) break;

        if(!flipper_format_read_uint32(ff, "Passive frames", &u32value, 1)) break;
        animation->passive_frames = u32value;
        if(!flipper_format_read_uint32(ff, "Active frames", &u32value, 1)) break;
        animation->active_frames = u32value;

        uint8_t frames = animation->passive_frames + animation->active_frames;
        uint32_t count = 0;
        if(!flipper_format_get_value_count(ff, "Frames order", &count)) break;
        if(count != frames) {
            FURI_LOG_E(TAG, "Error loading animation: frames order");
            break;
        }
        u32array = malloc(sizeof(uint32_t) * frames);
        if(!flipper_format_read_uint32(ff, "Frames order", u32array, frames)) break;
        animation->frame_order = malloc(sizeof(uint8_t) * frames);
        for(int i = 0; i < frames; ++i) {
            FURI_CONST_ASSIGN(animation->frame_order[i], u32array[i]);
        }

        /* passive and active frames must be loaded up to this point */
        if(!animation_storage_load_frames(storage, name, animation, u32array, width, height))
            break;

        if(!flipper_format_read_uint32(ff, "Active cycles", &u32value, 1)) break; //-V779
        animation->active_cycles = u32value;
        if(!flipper_format_read_uint32(ff, "Frame rate", &u32value, 1)) break;
        /* Keep the file's frame rate as is: Anim Speed is applied to the frame period in
         * bubble_animation_view.c (milliseconds, exact percentage), not here (whole fps
         * could not represent e.g. 2 fps x 125%). CLAMP is (x, upper, lower). */
        u32value = CLAMP(u32value, UINT8_MAX, 1);
        FURI_CONST_ASSIGN(animation->icon_animation.frame_rate, u32value);
        if(!flipper_format_read_uint32(ff, "Duration", &u32value, 1)) break;
        animation->duration = u32value;
        if(!flipper_format_read_uint32(ff, "Active cooldown", &u32value, 1)) break;
        animation->active_cooldown = u32value;

        if(!animation_storage_load_bubbles(animation, ff)) break;
        success = true;
    } while(0);

    furi_string_free(str);
    flipper_format_free(ff);
    if(u32array) {
        free(u32array);
    }

    if(!success) { //-V547
        /* Frames are loaded before the remaining meta.txt fields are parsed;
         * release them too, or every failed load of a broken animation leaks
         * all of its frame buffers. */
        animation_storage_free_bubbles(animation);
        animation_storage_free_frames(animation);
        if(animation->frame_order) {
            free((void*)animation->frame_order);
        }
        free(animation);
        animation = NULL;
    }

    return animation;
}

static void animation_storage_free_bubbles(BubbleAnimation* animation) {
    if(!animation->frame_bubble_sequences) return;

    for(int i = 0; i < animation->frame_bubble_sequences_count;) {
        const FrameBubble* const* bubble = &animation->frame_bubble_sequences[i];

        if((*bubble) == NULL) break;

        while((*bubble)->next_bubble != NULL) {
            bubble = &(*bubble)->next_bubble;
        }

        if((*bubble)->bubble.text) {
            free((void*)(*bubble)->bubble.text);
        }
        if((*bubble) == animation->frame_bubble_sequences[i]) {
            ++i;
        }
        free((void*)*bubble);
        FURI_CONST_ASSIGN_PTR(*bubble, NULL);
    }
    free((void*)animation->frame_bubble_sequences);
    animation->frame_bubble_sequences = NULL;
}

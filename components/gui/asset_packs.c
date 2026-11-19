#include "asset_packs.h"
#include "icon_i.h"
#include "momentum_settings.h"

#include <assets_icon_names.h>
#include <compress.h>
#include <furi.h>
#include <storage/storage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "AssetPacks"

#define ENTRY_NAME_LEN  64u
#define DIR_MAX_ENTRIES 512u

/* Pack bitmaps are drawn by the canvas decoder, whose output buffer holds one
 * 128x64 frame (ICON_DECOMPRESSOR_BUFFER_SIZE + 4). Anything larger would make
 * the draw path crash, so it is rejected here. */
#define ICON_MAX_WIDTH     128u
#define ICON_MAX_HEIGHT    64u
#define ICON_MAX_BITMAP    ((ICON_MAX_WIDTH / 8u) * ICON_MAX_HEIGHT)
#define ICON_DECODE_BUFFER (ICON_MAX_BITMAP + 4u)
#define BM_MIN_SIZE        4u /* the draw path reads a 4-byte CompressHeader */
#define BM_MAX_SIZE        (1u + ICON_MAX_BITMAP + 16u)
#define BMX_HEADER_SIZE    8u
/* Every file buffer gets this many zero bytes after its data. Some published
 * packs have raw bitmaps a byte short (e.g. BackToTheFlipper's happy
 * portrait); Momentum draws them by reading past the file. Here the missing
 * tail reads as zeros instead of out-of-bounds memory. */
#define READ_PADDING 16u
#define ICON_MAX_FRAMES    255u
#define ANIM_META_SIZE     16u

/* u8g2 font header (lib/u8g2/u8g2_font.c): 23 bytes, glyph table after it. */
#define FONT_HEADER_SIZE 23u
#define FONT_MAX_SIZE    (32u * 1024u)

/* Upper bound for everything one pack may keep in RAM, counted with padding
 * and an estimate of the heap's per-allocation overhead. Boards without PSRAM
 * (ESP32-C6, Cardputer) take this from internal RAM; the largest pack of the
 * Momentum catalog (Punch-Out) needs about 25 KB. */
#define PACK_MAX_BYTES     (64u * 1024u)
#define ALLOC_OVERHEAD     16u
#define ALLOC_COST(size)   ((size) + ALLOC_OVERHEAD)
#define BUFFER_COST(size)  ALLOC_COST((size) + READ_PADDING)

typedef struct {
    const Icon* original;
    const Icon* replaced;
} IconSwap;

typedef struct {
    char pack[ASSET_PACKS_NAME_LEN];
    IconSwap* icons; /* sorted by original pointer */
    size_t icon_count;
    const Icon* passport_background;
    const uint8_t* fonts[FontTotalNumber];
    CanvasFontParameters font_params[FontTotalNumber];
} AssetPacksState;

typedef struct {
    Icon icon;
    const uint8_t* frames[];
} PackIcon;

typedef struct {
    char name[ENTRY_NAME_LEN];
    bool is_dir;
} DirEntry;

/* "/ext/asset_packs/" + pack (< ASSET_PACKS_NAME_LEN) + "/Icons" fits 64. */
#define ICONS_PATH_LEN    64u
#define CATEGORY_PATH_LEN (ICONS_PATH_LEN + 1u + ENTRY_NAME_LEN)
#define ENTRY_PATH_LEN    (CATEGORY_PATH_LEN + 1u + ENTRY_NAME_LEN)

/* Heap-allocated: the loader runs on the desktop thread (4 KB stack), so path
 * buffers live here instead of on the stack. */
typedef struct {
    Storage* storage;
    File* file;
    Compress* compress;
    uint8_t* scratch;
    FuriString* path;
    AssetPacksState* state;
    size_t icon_capacity;
    size_t bytes;
    char icons_path[ICONS_PATH_LEN];
    char category_path[CATEGORY_PATH_LEN];
    char entry_path[ENTRY_PATH_LEN];
    char entry_name[ENTRY_NAME_LEN];
    char dir_name[ENTRY_NAME_LEN];
} Loader;

/* Written once (release) by asset_packs_init, read lock-free (acquire) by the
 * draw path. The state is immutable after publication and never freed. */
static AssetPacksState* asset_packs_state = NULL;
static bool asset_packs_attempted = false;

static const char* const font_names[FontTotalNumber] = {
    [FontPrimary] = "Primary",
    [FontSecondary] = "Secondary",
    [FontKeyboard] = "Keyboard",
    [FontBigNumbers] = "BigNumbers",
    [FontBatteryPercent] = "BatteryPercent",
};

/* Momentum names that map to firmware icons with a different name: the
 * firmware keeps three passport portraits per mood (one per level), Momentum
 * packs ship one. (Momentum's Levelup_128x64 is not mapped: the firmware's
 * level-up animations are never drawn, loading them would only cost RAM.) */
typedef struct {
    const char* pack_name;
    const char* targets[3];
} IconAlias;

static const IconAlias icon_aliases[] = {
    {"passport_happy_46x49",
     {"passport_happy1_46x49", "passport_happy2_46x49", "passport_happy3_46x49"}},
    {"passport_okay_46x49",
     {"passport_okay1_46x49", "passport_okay2_46x49", "passport_okay3_46x49"}},
    {"passport_bad_46x49", {"passport_bad1_46x49", "passport_bad2_46x49", "passport_bad3_46x49"}},
};

#define PASSPORT_BACKGROUND_NAME "passport_128x64"

static const AssetPacksState* asset_packs_get_state(void) {
    return __atomic_load_n(&asset_packs_state, __ATOMIC_ACQUIRE);
}

/* ---------------------------------------------------------------- loading */

static uint8_t*
    loader_read_file(Loader* loader, const char* path, size_t min_size, size_t max_size, size_t* out_size) {
    uint8_t* data = NULL;
    if(storage_file_open(loader->file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size = storage_file_size(loader->file);
        if(size < min_size || size > max_size) {
            /* wrong size for this kind of file: skip */
        } else if(loader->bytes + BUFFER_COST((size_t)size) > PACK_MAX_BYTES) {
            FURI_LOG_W(TAG, "RAM budget reached, skipping %s", path);
        } else {
            data = malloc((size_t)size + READ_PADDING);
            memset(&data[size], 0, READ_PADDING);
            if(storage_file_read(loader->file, data, (size_t)size) == size) {
                *out_size = (size_t)size;
                loader->bytes += BUFFER_COST((size_t)size);
            } else {
                free(data);
                data = NULL;
            }
        }
    }
    storage_file_close(loader->file);
    return data;
}

/* Free a buffer from loader_read_file and give its budget back. */
static void loader_release(Loader* loader, uint8_t* data, size_t size) {
    free(data);
    loader->bytes -= BUFFER_COST(size);
}

/* Check that a .bm bitmap (0x00 + raw, or 0x01 0x00 len16 + heatshrink) can be
 * drawn by the canvas decoder without failing or reading out of bounds. */
static bool
    loader_bitmap_valid(Loader* loader, const uint8_t* data, size_t size, uint32_t width, uint32_t height) {
    if(!width || !height || width > ICON_MAX_WIDTH || height > ICON_MAX_HEIGHT) return false;
    if(size < BM_MIN_SIZE) return false;
    const size_t expected = ((width + 7u) / 8u) * height;

    /* Raw data may be up to READ_PADDING bytes short: the zero padding after
     * the buffer (see loader_read_file) stands in for the missing tail. */
    if(data[0] == 0x00) return size + READ_PADDING >= 1u + expected;
    if(data[0] != 0x01) return false;

    const size_t compressed = (size_t)data[2] | ((size_t)data[3] << 8);
    if(!compressed || 4u + compressed > size) return false;

    size_t decoded = 0;
    if(!compress_decode(
           loader->compress, (uint8_t*)data, size, loader->scratch, ICON_DECODE_BUFFER, &decoded)) {
        return false;
    }
    return decoded >= expected;
}

static PackIcon* pack_icon_alloc(uint32_t width, uint32_t height, uint32_t frames, uint32_t rate) {
    PackIcon* pack_icon = malloc(sizeof(PackIcon) + sizeof(const uint8_t*) * frames);
    memset(pack_icon->frames, 0, sizeof(const uint8_t*) * frames);
    const Icon icon = {
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .frame_count = (uint8_t)frames,
        .frame_rate = (uint8_t)rate,
        .frames = pack_icon->frames,
    };
    memcpy(&pack_icon->icon, &icon, sizeof(icon));
    return pack_icon;
}

static const Icon* loader_load_static(Loader* loader, const char* path) {
    size_t size = 0;
    uint8_t* data =
        loader_read_file(loader, path, BMX_HEADER_SIZE + BM_MIN_SIZE, BMX_HEADER_SIZE + BM_MAX_SIZE, &size);
    if(!data) return NULL;

    uint32_t width = 0;
    uint32_t height = 0;
    memcpy(&width, &data[0], sizeof(width));
    memcpy(&height, &data[4], sizeof(height));
    if(!loader_bitmap_valid(loader, &data[BMX_HEADER_SIZE], size - BMX_HEADER_SIZE, width, height)) {
        FURI_LOG_W(TAG, "Invalid icon %s", path);
        loader_release(loader, data, size);
        return NULL;
    }

    PackIcon* pack_icon = pack_icon_alloc(width, height, 1, 0);
    /* The frame points into `data`, which stays allocated with the icon. */
    pack_icon->frames[0] = &data[BMX_HEADER_SIZE];
    loader->bytes += ALLOC_COST(sizeof(PackIcon) + sizeof(const uint8_t*));
    return &pack_icon->icon;
}

static const Icon* loader_load_animated(Loader* loader, const char* dir) {
    furi_string_printf(loader->path, "%s/meta", dir);
    size_t meta_size = 0;
    uint8_t* meta = loader_read_file(
        loader, furi_string_get_cstr(loader->path), ANIM_META_SIZE, ANIM_META_SIZE, &meta_size);
    if(!meta) return NULL;

    uint32_t values[4];
    memcpy(values, meta, sizeof(values));
    loader_release(loader, meta, meta_size);
    const uint32_t width = values[0];
    const uint32_t height = values[1];
    const uint32_t frame_rate = values[2];
    const uint32_t frame_count = values[3];
    if(!frame_count || frame_count > ICON_MAX_FRAMES || !frame_rate || frame_rate > UINT8_MAX) {
        FURI_LOG_W(TAG, "Invalid animated icon meta %s", dir);
        return NULL;
    }

    PackIcon* pack_icon = pack_icon_alloc(width, height, frame_count, frame_rate);
    size_t frames_bytes = 0;
    uint32_t loaded = 0;
    for(; loaded < frame_count; loaded++) {
        furi_string_printf(loader->path, "%s/frame_%02lu.bm", dir, (unsigned long)loaded);
        size_t size = 0;
        uint8_t* frame = loader_read_file(
            loader, furi_string_get_cstr(loader->path), BM_MIN_SIZE, BM_MAX_SIZE, &size);
        if(!frame) break;
        if(!loader_bitmap_valid(loader, frame, size, width, height)) {
            loader_release(loader, frame, size);
            break;
        }
        pack_icon->frames[loaded] = frame;
        frames_bytes += BUFFER_COST(size);
    }

    if(loaded != frame_count) {
        FURI_LOG_W(TAG, "Invalid animated icon %s (frame %lu)", dir, (unsigned long)loaded);
        for(uint32_t i = 0; i < loaded; i++) {
            free((void*)pack_icon->frames[i]);
        }
        loader->bytes -= frames_bytes;
        free(pack_icon);
        return NULL;
    }

    loader->bytes += ALLOC_COST(sizeof(PackIcon) + sizeof(const uint8_t*) * frame_count);
    return &pack_icon->icon;
}

static int icon_name_compare(const void* key, const void* item) {
    return strcmp((const char*)key, ((const AssetIconName*)item)->name);
}

static const Icon* find_firmware_icon(const char* name) {
    const AssetIconName* found = bsearch(
        name, assets_icon_names, assets_icon_names_count, sizeof(AssetIconName), icon_name_compare);
    return found ? found->icon : NULL;
}

static bool loader_is_swapped(const Loader* loader, const Icon* original) {
    for(size_t i = 0; i < loader->state->icon_count; i++) {
        if(loader->state->icons[i].original == original) return true;
    }
    return false;
}

static void loader_add_swap(Loader* loader, const Icon* original, const Icon* replaced) {
    AssetPacksState* state = loader->state;
    if(state->icon_count == loader->icon_capacity) {
        loader->icon_capacity = loader->icon_capacity ? loader->icon_capacity * 2 : 32;
        state->icons = realloc(state->icons, sizeof(IconSwap) * loader->icon_capacity);
    }
    state->icons[state->icon_count++] = (IconSwap){.original = original, .replaced = replaced};
}

/* One entry of Icons/<category>/: "<name>.bmx" (static) or "<name>/" (animated). */
static void loader_load_icon_entry(Loader* loader, const DirEntry* entry) {
    char* name = loader->entry_name;
    strlcpy(name, entry->name, sizeof(loader->entry_name));
    if(!entry->is_dir) {
        const size_t len = strlen(name);
        if(len <= 4 || strcmp(&name[len - 4], ".bmx") != 0) return;
        name[len - 4] = '\0';
    }

    char* entry_path = loader->entry_path;
    snprintf(
        entry_path, sizeof(loader->entry_path), "%s/%s", loader->category_path, entry->name);

    if(!entry->is_dir && strcmp(name, PASSPORT_BACKGROUND_NAME) == 0) {
        if(!loader->state->passport_background) {
            loader->state->passport_background = loader_load_static(loader, entry_path);
        }
        return;
    }

    const char* single[3] = {name, NULL, NULL};
    const char* const* targets = single;
    for(size_t i = 0; i < COUNT_OF(icon_aliases); i++) {
        if(strcmp(name, icon_aliases[i].pack_name) == 0) {
            targets = icon_aliases[i].targets;
            break;
        }
    }

    /* Replace like with like: a static file never stands in for an animated
     * firmware icon (IconAnimation needs a frame rate) and vice versa. */
    const Icon* originals[3];
    size_t original_count = 0;
    for(size_t i = 0; i < 3 && targets[i]; i++) {
        const Icon* original = find_firmware_icon(targets[i]);
        if(!original || loader_is_swapped(loader, original)) continue;
        if((original->frame_count > 1) != entry->is_dir) continue;
        originals[original_count++] = original;
    }
    if(!original_count) return;

    const Icon* replaced = entry->is_dir ? loader_load_animated(loader, entry_path) :
                                           loader_load_static(loader, entry_path);
    if(!replaced) return;
    for(size_t i = 0; i < original_count; i++) {
        loader_add_swap(loader, originals[i], replaced);
    }
}

/* Read a directory into a list so no directory handle stays open while files
 * are read. Hidden entries are skipped. */
static DirEntry* loader_list_dir(Loader* loader, const char* path, size_t* count) {
    *count = 0;
    size_t capacity = 0;
    DirEntry* entries = NULL;
    File* dir = storage_file_alloc(loader->storage);
    if(storage_dir_open(dir, path)) {
        FileInfo info;
        char* name = loader->dir_name;
        while(*count < DIR_MAX_ENTRIES &&
              storage_dir_read(dir, &info, name, sizeof(loader->dir_name))) {
            if(name[0] == '.') continue;
            if(*count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                entries = realloc(entries, sizeof(DirEntry) * capacity);
            }
            strlcpy(entries[*count].name, name, ENTRY_NAME_LEN);
            entries[*count].is_dir = (info.flags & FSF_DIRECTORY) != 0;
            (*count)++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return entries;
}

static void loader_load_icons(Loader* loader) {
    snprintf(
        loader->icons_path,
        sizeof(loader->icons_path),
        "%s/%s/Icons",
        ASSET_PACKS_PATH,
        loader->state->pack);

    size_t category_count = 0;
    DirEntry* categories = loader_list_dir(loader, loader->icons_path, &category_count);
    for(size_t c = 0; c < category_count; c++) {
        if(!categories[c].is_dir) continue;
        snprintf(
            loader->category_path,
            sizeof(loader->category_path),
            "%s/%s",
            loader->icons_path,
            categories[c].name);

        size_t entry_count = 0;
        DirEntry* entries = loader_list_dir(loader, loader->category_path, &entry_count);
        for(size_t e = 0; e < entry_count; e++) {
            loader_load_icon_entry(loader, &entries[e]);
        }
        free(entries);
    }
    free(categories);
}

/* ------------------------------------------------------- font validation
 * u8g2 trusts its font data completely, so a broken .u8f would make every
 * text draw read past the heap buffer or loop forever. These checks replay
 * exactly what lib u8g2_font.c reads (glyph lookup and glyph decode) and
 * reject the font if any of it would leave the file. They guarantee memory
 * safety, not pretty glyphs. */

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    uint8_t bit;
    bool overrun;
} FontBits;

/* Same reads as u8g2_font_decode_get_unsigned_bits (cnt <= 8). */
static uint8_t font_bits_get(FontBits* bits, uint8_t cnt) {
    if(bits->overrun || bits->pos >= bits->size) {
        bits->overrun = true;
        return 0;
    }
    uint32_t value = bits->data[bits->pos] >> bits->bit;
    uint8_t next = bits->bit + cnt;
    if(next >= 8) {
        const uint8_t shift = 8 - bits->bit;
        bits->pos++;
        if(bits->pos >= bits->size) {
            bits->overrun = true;
            return 0;
        }
        value |= (uint32_t)bits->data[bits->pos] << shift;
        next -= 8;
    }
    bits->bit = next;
    return (uint8_t)(value & ((1U << cnt) - 1U));
}

static void font_advance(uint32_t* x, uint32_t* y, uint32_t len, uint32_t width) {
    *x += len;
    while(*x >= width) {
        *x -= width;
        (*y)++;
    }
}

/* Replay u8g2_font_setup_decode + u8g2_font_decode_glyph for one glyph. */
static bool font_glyph_valid(const uint8_t* data, size_t size, size_t glyph) {
    FontBits bits = {.data = data, .size = size, .pos = glyph};
    const uint32_t width = font_bits_get(&bits, data[4]);
    const uint32_t height = font_bits_get(&bits, data[5]);
    if(width >= 128 || height >= 128) return false; /* u8g2 keeps them in int8_t */
    font_bits_get(&bits, data[6]); /* x */
    font_bits_get(&bits, data[7]); /* y */
    font_bits_get(&bits, data[8]); /* delta x */
    if(width) {
        uint32_t x = 0;
        uint32_t y = 0;
        /* Every pass reads at least one bit, so overrun ends a broken glyph. */
        while(!bits.overrun) {
            const uint8_t zeros = font_bits_get(&bits, data[2]);
            const uint8_t ones = font_bits_get(&bits, data[3]);
            do {
                font_advance(&x, &y, zeros, width);
                font_advance(&x, &y, ones, width);
            } while(font_bits_get(&bits, 1) != 0 && !bits.overrun);
            if(y >= height) break;
        }
    }
    return !bits.overrun;
}

static size_t font_word(const uint8_t* data, size_t pos) {
    return ((size_t)data[pos] << 8) | data[pos + 1];
}

/* 8-bit glyph list walked from `start` as u8g2_font_get_glyph_data does. */
static bool font_list_valid(const uint8_t* data, size_t size, size_t start) {
    size_t pos = FONT_HEADER_SIZE + start;
    for(size_t guard = 0; guard < size; guard++) {
        if(pos + 1 >= size) return false;
        const uint8_t jump = data[pos + 1];
        if(jump == 0) return true;
        if(!font_glyph_valid(data, size, pos + 2)) return false;
        pos += jump;
    }
    return false;
}

/* Unicode lookup table (offset/encoding words up to 0xFFFF) and the glyph
 * list each entry points into. */
static bool font_unicode_valid(const uint8_t* data, size_t size) {
    size_t table = FONT_HEADER_SIZE + font_word(data, 21);
    size_t glyphs = table;
    for(size_t entry_guard = 0; entry_guard < size; entry_guard++) {
        if(table + 4 > size) return false;
        glyphs += font_word(data, table);
        const size_t encoding = font_word(data, table + 2);
        table += 4;

        size_t pos = glyphs;
        for(size_t guard = 0;; guard++) {
            if(guard >= size || pos + 1 >= size) return false;
            if(font_word(data, pos) == 0) break;
            if(pos + 2 >= size || data[pos + 2] == 0) return false; /* 0 would loop forever */
            if(!font_glyph_valid(data, size, pos + 3)) return false;
            pos += data[pos + 2];
        }
        if(encoding == 0xFFFF) return true;
    }
    return false;
}

static bool font_valid(const uint8_t* data, size_t size) {
    if(size <= FONT_HEADER_SIZE || data[0] == 0 || data[size - 1] != 0) return false;
    for(size_t i = 2; i <= 8; i++) {
        if(data[i] > 8) return false; /* bit field widths, read 8 bits at most */
    }
    return font_list_valid(data, size, 0) && font_list_valid(data, size, font_word(data, 17)) &&
           font_list_valid(data, size, font_word(data, 19)) && font_unicode_valid(data, size);
}

static void loader_load_fonts(Loader* loader) {
    for(Font font = 0; font < FontTotalNumber; font++) {
        furi_string_printf(
            loader->path, "%s/%s/Fonts/%s.u8f", ASSET_PACKS_PATH, loader->state->pack, font_names[font]);
        size_t size = 0;
        uint8_t* data = loader_read_file(
            loader, furi_string_get_cstr(loader->path), FONT_HEADER_SIZE + 1, FONT_MAX_SIZE, &size);
        if(!data) continue;
        if(!font_valid(data, size)) {
            FURI_LOG_W(TAG, "Invalid font %s", furi_string_get_cstr(loader->path));
            loader_release(loader, data, size);
            continue;
        }
        /* Same derivation as Momentum: max_char_height, ascent/descent_para. */
        const int8_t ascent = (int8_t)data[15];
        const int8_t descent = (int8_t)data[16];
        CanvasFontParameters* params = &loader->state->font_params[font];
        params->leading_default = data[10];
        params->leading_min = data[10] > 2 ? data[10] - 2 : data[10];
        params->height = ascent > 0 ? (uint8_t)ascent : 0;
        params->descender = descent > 0 ? (uint8_t)descent : 0;
        loader->state->fonts[font] = data;
    }
}

static int icon_swap_compare(const void* a, const void* b) {
    const uintptr_t left = (uintptr_t)((const IconSwap*)a)->original;
    const uintptr_t right = (uintptr_t)((const IconSwap*)b)->original;
    return (left > right) - (left < right);
}

void asset_packs_init(void) {
    if(__atomic_load_n(&asset_packs_attempted, __ATOMIC_ACQUIRE)) return;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage_sd_status(storage) != FSE_OK ||
       __atomic_exchange_n(&asset_packs_attempted, true, __ATOMIC_ACQ_REL)) {
        furi_record_close(RECORD_STORAGE);
        return;
    }

    AssetPacksState* state = malloc(sizeof(AssetPacksState));
    memset(state, 0, sizeof(AssetPacksState));
    momentum_settings_get_asset_pack(state->pack, sizeof(state->pack));

    if(state->pack[0] != '\0') {
        Loader* loader = malloc(sizeof(Loader));
        memset(loader, 0, sizeof(Loader));
        loader->storage = storage;
        loader->file = storage_file_alloc(storage);
        loader->compress =
            compress_alloc(CompressTypeHeatshrink, &compress_config_heatshrink_default);
        loader->scratch = malloc(ICON_DECODE_BUFFER);
        loader->path = furi_string_alloc();
        loader->state = state;

        loader_load_icons(loader);
        loader_load_fonts(loader);
        const size_t loaded_bytes = loader->bytes;

        furi_string_free(loader->path);
        free(loader->scratch);
        compress_free(loader->compress);
        storage_file_free(loader->file);
        free(loader);

        if(state->icon_count) {
            qsort(state->icons, state->icon_count, sizeof(IconSwap), icon_swap_compare);
        }
        size_t font_count = 0;
        for(Font font = 0; font < FontTotalNumber; font++) {
            font_count += state->fonts[font] != NULL;
        }
        FURI_LOG_I(
            TAG,
            "'%s': %u icons, %u fonts, passport background %s, %u bytes",
            state->pack,
            (unsigned)state->icon_count,
            (unsigned)font_count,
            state->passport_background ? "yes" : "no",
            (unsigned)loaded_bytes);
    }

    __atomic_store_n(&asset_packs_state, state, __ATOMIC_RELEASE);
    furi_record_close(RECORD_STORAGE);
}

/* ---------------------------------------------------------------- lookups */

const Icon* asset_packs_swap_icon(const Icon* requested) {
    const AssetPacksState* state = asset_packs_get_state();
    if(!state || !state->icon_count || !requested) return requested;

    size_t low = 0;
    size_t high = state->icon_count;
    const uintptr_t key = (uintptr_t)requested;
    while(low < high) {
        const size_t mid = low + (high - low) / 2;
        const uintptr_t original = (uintptr_t)state->icons[mid].original;
        if(original == key) return state->icons[mid].replaced;
        if(original < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return requested;
}

const uint8_t* asset_packs_get_font(Font font) {
    const AssetPacksState* state = asset_packs_get_state();
    if(!state || font >= FontTotalNumber) return NULL;
    return state->fonts[font];
}

const CanvasFontParameters* asset_packs_get_font_params(Font font) {
    const AssetPacksState* state = asset_packs_get_state();
    if(!state || font >= FontTotalNumber || !state->fonts[font]) return NULL;
    return &state->font_params[font];
}

const Icon* asset_packs_get_passport_background(void) {
    const AssetPacksState* state = asset_packs_get_state();
    return state ? state->passport_background : NULL;
}

static bool pack_has_dir(Storage* storage, const char* pack, const char* dir) {
    FuriString* path = furi_string_alloc_printf("%s/%s/%s", ASSET_PACKS_PATH, pack, dir);
    FileInfo info;
    const bool exists = storage_common_stat(storage, furi_string_get_cstr(path), &info) == FSE_OK &&
                        (info.flags & FSF_DIRECTORY);
    furi_string_free(path);
    return exists;
}

bool asset_packs_restart_required(const char* selected_pack) {
    if(!selected_pack) selected_pack = "";
    const AssetPacksState* state = asset_packs_get_state();
    const char* applied = state ? state->pack : "";
    if(strcmp(applied, selected_pack) == 0) return false;

    if(state) {
        bool has_fonts = false;
        for(Font font = 0; font < FontTotalNumber; font++) {
            has_fonts |= state->fonts[font] != NULL;
        }
        if(state->icon_count || state->passport_background || has_fonts) return true;
    }
    if(selected_pack[0] == '\0') return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    const bool has_assets =
        pack_has_dir(storage, selected_pack, "Icons") || pack_has_dir(storage, selected_pack, "Fonts");
    furi_record_close(RECORD_STORAGE);
    return has_assets;
}

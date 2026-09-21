#include "menu_custom.h"
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define TAG "MenuCustom"

#define MENU_CUSTOM_DIR  EXT_PATH("apps_data")
#define MENU_CUSTOM_PATH MENU_CUSTOM_DIR "/" MENU_CUSTOM_FILENAME

void menu_custom_load(MenuCustom* custom) {
    furi_check(custom);
    memset(custom, 0, sizeof(MenuCustom));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);

    if(file_stream_open(stream, MENU_CUSTOM_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FuriString* line = furi_string_alloc();
        while(stream_read_line(stream, line)) {
            furi_string_trim(line);
            if(furi_string_start_with(line, "-")) {
                if(custom->hidden_count < MENU_CUSTOM_MAX_ITEMS) {
                    custom->hidden[custom->hidden_count++] =
                        furi_string_alloc_set(furi_string_get_cstr(line) + 1);
                }
            } else if(furi_string_start_with(line, "+")) {
                if(custom->added_fap_count < MENU_CUSTOM_MAX_ITEMS) {
                    custom->added_fap[custom->added_fap_count++] =
                        furi_string_alloc_set(furi_string_get_cstr(line) + 1);
                }
            } else if(furi_string_start_with(line, "=")) {
                if(custom->added_internal_count < MENU_CUSTOM_MAX_ITEMS) {
                    custom->added_internal[custom->added_internal_count++] =
                        furi_string_alloc_set(furi_string_get_cstr(line) + 1);
                }
            } else if(furi_string_start_with(line, ">")) {
                if(custom->order_count < MENU_CUSTOM_MAX_ITEMS) {
                    custom->order[custom->order_count++] =
                        furi_string_alloc_set(furi_string_get_cstr(line) + 1);
                }
            }
        }
        furi_string_free(line);
    } else {
        FURI_LOG_D(TAG, "No custom menu file (%s), using defaults", MENU_CUSTOM_PATH);
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

void menu_custom_save(const MenuCustom* custom) {
    furi_check(custom);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, MENU_CUSTOM_DIR);

    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, MENU_CUSTOM_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        for(size_t i = 0; i < custom->hidden_count; ++i) {
            stream_write_format(stream, "-%s\n", furi_string_get_cstr(custom->hidden[i]));
        }
        for(size_t i = 0; i < custom->added_internal_count; ++i) {
            stream_write_format(stream, "=%s\n", furi_string_get_cstr(custom->added_internal[i]));
        }
        for(size_t i = 0; i < custom->added_fap_count; ++i) {
            stream_write_format(stream, "+%s\n", furi_string_get_cstr(custom->added_fap[i]));
        }
        for(size_t i = 0; i < custom->order_count; ++i) {
            stream_write_format(stream, ">%s\n", furi_string_get_cstr(custom->order[i]));
        }
    } else {
        FURI_LOG_E(TAG, "Failed to open %s for write", MENU_CUSTOM_PATH);
    }
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static bool menu_custom_string_in(
    FuriString* const* list,
    size_t count,
    const char* value) {
    for(size_t i = 0; i < count; ++i) {
        if(furi_string_cmp_str(list[i], value) == 0) {
            return true;
        }
    }
    return false;
}

bool menu_custom_is_hidden(const MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    return menu_custom_string_in(custom->hidden, custom->hidden_count, app_id);
}

bool menu_custom_is_fap_added(const MenuCustom* custom, const char* fap_path) {
    furi_check(custom);
    return menu_custom_string_in(custom->added_fap, custom->added_fap_count, fap_path);
}

bool menu_custom_is_internal_added(const MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    return menu_custom_string_in(custom->added_internal, custom->added_internal_count, app_id);
}

bool menu_custom_hide(MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    if(menu_custom_is_hidden(custom, app_id)) return false;
    if(custom->hidden_count >= MENU_CUSTOM_MAX_ITEMS) return false;
    custom->hidden[custom->hidden_count++] = furi_string_alloc_set(app_id);
    return true;
}

bool menu_custom_unhide(MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    for(size_t i = 0; i < custom->hidden_count; ++i) {
        if(furi_string_cmp_str(custom->hidden[i], app_id) == 0) {
            furi_string_free(custom->hidden[i]);
            custom->hidden[i] = custom->hidden[--custom->hidden_count];
            return true;
        }
    }
    return false;
}

bool menu_custom_add_fap(MenuCustom* custom, const char* fap_path) {
    furi_check(custom);
    if(menu_custom_is_fap_added(custom, fap_path)) return false;
    if(custom->added_fap_count >= MENU_CUSTOM_MAX_ITEMS) return false;
    custom->added_fap[custom->added_fap_count++] = furi_string_alloc_set(fap_path);
    return true;
}

bool menu_custom_remove_fap(MenuCustom* custom, const char* fap_path) {
    furi_check(custom);
    for(size_t i = 0; i < custom->added_fap_count; ++i) {
        if(furi_string_cmp_str(custom->added_fap[i], fap_path) == 0) {
            furi_string_free(custom->added_fap[i]);
            custom->added_fap[i] = custom->added_fap[--custom->added_fap_count];
            return true;
        }
    }
    return false;
}

bool menu_custom_add_internal(MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    if(menu_custom_is_internal_added(custom, app_id)) return false;
    if(custom->added_internal_count >= MENU_CUSTOM_MAX_ITEMS) return false;
    custom->added_internal[custom->added_internal_count++] = furi_string_alloc_set(app_id);
    return true;
}

bool menu_custom_remove_internal(MenuCustom* custom, const char* app_id) {
    furi_check(custom);
    for(size_t i = 0; i < custom->added_internal_count; ++i) {
        if(furi_string_cmp_str(custom->added_internal[i], app_id) == 0) {
            furi_string_free(custom->added_internal[i]);
            custom->added_internal[i] = custom->added_internal[--custom->added_internal_count];
            return true;
        }
    }
    return false;
}

void menu_custom_set_order(MenuCustom* custom, const char* const* keys, size_t count) {
    furi_check(custom);
    for(size_t i = 0; i < custom->order_count; ++i) {
        furi_string_free(custom->order[i]);
    }
    custom->order_count = 0;
    for(size_t i = 0; i < count && custom->order_count < MENU_CUSTOM_MAX_ITEMS; ++i) {
        custom->order[custom->order_count++] = furi_string_alloc_set(keys[i]);
    }
}

/* Zwolnienie wszystkich alokowanych stringow (wolne w menu_custom_free). */
void menu_custom_free(MenuCustom* custom) {
    if(!custom) return;
    for(size_t i = 0; i < custom->hidden_count; ++i) {
        furi_string_free(custom->hidden[i]);
    }
    for(size_t i = 0; i < custom->added_fap_count; ++i) {
        furi_string_free(custom->added_fap[i]);
    }
    for(size_t i = 0; i < custom->added_internal_count; ++i) {
        furi_string_free(custom->added_internal[i]);
    }
    for(size_t i = 0; i < custom->order_count; ++i) {
        furi_string_free(custom->order[i]);
    }
    memset(custom, 0, sizeof(MenuCustom));
}
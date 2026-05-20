#include "desktop_keybinds.h"

#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#define TAG "DesktopKeybinds"

static const char* const default_actions[DesktopKeybindTypeMAX][DesktopKeybindKeyMAX] = {
    [DesktopKeybindTypePress] = {
        [DesktopKeybindKeyUp] = "Lock Menu",
        [DesktopKeybindKeyDown] = "Archive",
        [DesktopKeybindKeyRight] = "Passport",
        [DesktopKeybindKeyLeft] = "Clock",
    },
    [DesktopKeybindTypeHold] = {
        [DesktopKeybindKeyUp] = "",
        [DesktopKeybindKeyDown] = "",
        [DesktopKeybindKeyRight] = "Device Info",
        [DesktopKeybindKeyLeft] = "Lock with PIN",
    },
};

DesktopKeybinds* desktop_keybinds_alloc(void) {
    DesktopKeybinds* keybinds = malloc(sizeof(DesktopKeybinds));
    for(int t = 0; t < DesktopKeybindTypeMAX; t++) {
        for(int k = 0; k < DesktopKeybindKeyMAX; k++) {
            keybinds->actions[t][k] = NULL;
        }
    }
    return keybinds;
}

void desktop_keybinds_free(DesktopKeybinds* keybinds) {
    for(int t = 0; t < DesktopKeybindTypeMAX; t++) {
        for(int k = 0; k < DesktopKeybindKeyMAX; k++) {
            if(keybinds->actions[t][k]) {
                free(keybinds->actions[t][k]);
            }
        }
    }
    free(keybinds);
}

void desktop_keybinds_set_default(DesktopKeybinds* keybinds) {
    for(int t = 0; t < DesktopKeybindTypeMAX; t++) {
        for(int k = 0; k < DesktopKeybindKeyMAX; k++) {
            if(keybinds->actions[t][k]) {
                free(keybinds->actions[t][k]);
            }
            keybinds->actions[t][k] =
                default_actions[t][k][0] ? strdup(default_actions[t][k]) : NULL;
        }
    }
}

void desktop_keybinds_load(DesktopKeybinds* keybinds) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    desktop_keybinds_set_default(keybinds);

    do {
        if(!flipper_format_file_open_existing(ff, DESKTOP_KEYBINDS_PATH)) break;

        uint32_t version = 0;
        if(!flipper_format_read_uint32(ff, "Version", &version, 1)) break;
        if(version != 1) break;

        FuriString* value = furi_string_alloc();
        for(int t = 0; t < DesktopKeybindTypeMAX; t++) {
            for(int k = 0; k < DesktopKeybindKeyMAX; k++) {
                const char* key_name = NULL;
                if(t == DesktopKeybindTypePress) {
                    static const char* press_names[] = {"Up", "Down", "Right", "Left"};
                    key_name = press_names[k];
                } else {
                    static const char* hold_names[] = {"Up_Hold", "Down_Hold", "Right_Hold", "Left_Hold"};
                    key_name = hold_names[k];
                }
                if(flipper_format_read_string(ff, key_name, value)) {
                    if(keybinds->actions[t][k]) free(keybinds->actions[t][k]);
                    keybinds->actions[t][k] =
                        furi_string_empty(value) ? NULL : strdup(furi_string_get_cstr(value));
                }
            }
        }
        furi_string_free(value);
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

void desktop_keybinds_save(DesktopKeybinds* keybinds) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);

    do {
        if(!flipper_format_file_open_always(ff, DESKTOP_KEYBINDS_PATH)) break;

        uint32_t version = 1;
        flipper_format_write_header_cstr(ff, "DesktopKeybinds", version);

        for(int t = 0; t < DesktopKeybindTypeMAX; t++) {
            for(int k = 0; k < DesktopKeybindKeyMAX; k++) {
                const char* key_name = NULL;
                if(t == DesktopKeybindTypePress) {
                    static const char* press_names[] = {"Up", "Down", "Right", "Left"};
                    key_name = press_names[k];
                } else {
                    static const char* hold_names[] = {"Up_Hold", "Down_Hold", "Right_Hold", "Left_Hold"};
                    key_name = hold_names[k];
                }
                const char* val = keybinds->actions[t][k];
                if(val) {
                    flipper_format_write_string_cstr(ff, key_name, val);
                }
            }
        }
    } while(false);

    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
}

const char* desktop_keybinds_get_action(
    DesktopKeybinds* keybinds,
    DesktopKeybindType type,
    DesktopKeybindKey key) {
    if(type >= DesktopKeybindTypeMAX || key >= DesktopKeybindKeyMAX) return NULL;
    return keybinds->actions[type][key];
}

void desktop_keybinds_set_action(
    DesktopKeybinds* keybinds,
    DesktopKeybindType type,
    DesktopKeybindKey key,
    const char* action) {
    if(type >= DesktopKeybindTypeMAX || key >= DesktopKeybindKeyMAX) return;
    if(keybinds->actions[type][key]) free(keybinds->actions[type][key]);
    keybinds->actions[type][key] = action ? strdup(action) : NULL;
}

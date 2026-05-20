#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DESKTOP_KEYBINDS_PATH INT_PATH(".desktop_keybinds.txt")

typedef enum {
    DesktopKeybindKeyUp,
    DesktopKeybindKeyDown,
    DesktopKeybindKeyRight,
    DesktopKeybindKeyLeft,
    DesktopKeybindKeyOk,
    DesktopKeybindKeyBack,
    DesktopKeybindKeyMAX,
} DesktopKeybindKey;

typedef enum {
    DesktopKeybindTypePress,
    DesktopKeybindTypeHold,
    DesktopKeybindTypeMAX,
} DesktopKeybindType;

typedef struct {
    char* actions[DesktopKeybindTypeMAX][DesktopKeybindKeyMAX];
} DesktopKeybinds;

DesktopKeybinds* desktop_keybinds_alloc(void);
void desktop_keybinds_free(DesktopKeybinds* keybinds);
void desktop_keybinds_load(DesktopKeybinds* keybinds);
void desktop_keybinds_save(DesktopKeybinds* keybinds);
void desktop_keybinds_set_default(DesktopKeybinds* keybinds);
const char* desktop_keybinds_get_action(
    DesktopKeybinds* keybinds,
    DesktopKeybindType type,
    DesktopKeybindKey key);
void desktop_keybinds_set_action(
    DesktopKeybinds* keybinds,
    DesktopKeybindType type,
    DesktopKeybindKey key,
    const char* action);

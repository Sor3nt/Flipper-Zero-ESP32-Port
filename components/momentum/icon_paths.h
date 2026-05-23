#pragma once

#include <stddef.h>
#include <gui/icon.h>

typedef struct {
    const Icon* icon;
    const char* path;
} IconPath;

extern const IconPath ICON_PATHS[];
extern const size_t ICON_PATHS_COUNT;

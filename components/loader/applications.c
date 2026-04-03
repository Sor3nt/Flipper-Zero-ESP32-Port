#include "applications.h"
#include <assets_icons.h>

/* App entry points */
extern int32_t about_app(void* p);
extern int32_t archive_app(void* p);

#define FLIPPER_ARCHIVE_DEF                     \
    {                                           \
        .appid = "archive",                     \
        .name = "Archive",                      \
        .icon = &A_FileManager_14,              \
        .app = archive_app,                     \
        .stack_size = 8192,                     \
        .flags = FlipperInternalApplicationFlagDefault, \
    }

const FlipperInternalApplication FLIPPER_APPS[] = {
    {
        .appid = "about",
        .name = "About",
        .icon = &A_Settings_14,
        .app = about_app,
        .stack_size = 4096,
        .flags = FlipperInternalApplicationFlagDefault,
    },
};
const size_t FLIPPER_APPS_COUNT = COUNT_OF(FLIPPER_APPS);

const FlipperInternalApplication FLIPPER_SETTINGS_APPS[] = {};
const size_t FLIPPER_SETTINGS_APPS_COUNT = 0;

const FlipperInternalApplication FLIPPER_SYSTEM_APPS[] = {FLIPPER_ARCHIVE_DEF};
const size_t FLIPPER_SYSTEM_APPS_COUNT = COUNT_OF(FLIPPER_SYSTEM_APPS);

const FlipperInternalApplication FLIPPER_DEBUG_APPS[] = {};
const size_t FLIPPER_DEBUG_APPS_COUNT = COUNT_OF(FLIPPER_DEBUG_APPS);

const FlipperInternalApplication FLIPPER_ARCHIVE = FLIPPER_ARCHIVE_DEF;

const FlipperExternalApplication FLIPPER_EXTSETTINGS_APPS[] = {};
const size_t FLIPPER_EXTSETTINGS_APPS_COUNT = COUNT_OF(FLIPPER_EXTSETTINGS_APPS);

const FlipperExternalApplication FLIPPER_EXTERNAL_APPS[] = {};
const size_t FLIPPER_EXTERNAL_APPS_COUNT = COUNT_OF(FLIPPER_EXTERNAL_APPS);

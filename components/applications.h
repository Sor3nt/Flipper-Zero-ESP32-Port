#pragma once

#include <furi.h>

typedef struct Icon Icon;

typedef void (*FlipperInternalOnStartHook)(void);

typedef enum {
    FlipperInternalApplicationFlagDefault = 0,
    FlipperInternalApplicationFlagInsomniaSafe = (1 << 0),
} FlipperInternalApplicationFlag;

typedef struct {
    const FuriThreadCallback app;
    const char* name;
    const char* appid;
    const size_t stack_size;
    const Icon* icon;
    const FlipperInternalApplicationFlag flags;
} FlipperInternalApplication;

typedef struct {
    const char* name;
    const Icon* icon;
    const char* path;
} FlipperExternalApplication;

extern const char* FLIPPER_AUTORUN_APP_NAME;

extern const FlipperInternalApplication FLIPPER_SERVICES[];
extern const size_t FLIPPER_SERVICES_COUNT;

extern const FlipperInternalApplication FLIPPER_APPS[];
extern const size_t FLIPPER_APPS_COUNT;

extern const FlipperInternalOnStartHook FLIPPER_ON_SYSTEM_START[];
extern const size_t FLIPPER_ON_SYSTEM_START_COUNT;

extern const FlipperInternalApplication FLIPPER_SETTINGS_APPS[];
extern const size_t FLIPPER_SETTINGS_APPS_COUNT;

extern const FlipperInternalApplication FLIPPER_SYSTEM_APPS[];
extern const size_t FLIPPER_SYSTEM_APPS_COUNT;

extern const FlipperInternalApplication FLIPPER_DEBUG_APPS[];
extern const size_t FLIPPER_DEBUG_APPS_COUNT;

extern const FlipperInternalApplication FLIPPER_ARCHIVE;

extern const FlipperExternalApplication FLIPPER_EXTSETTINGS_APPS[];
extern const size_t FLIPPER_EXTSETTINGS_APPS_COUNT;

extern const FlipperExternalApplication FLIPPER_EXTERNAL_APPS[];
extern const size_t FLIPPER_EXTERNAL_APPS_COUNT;

extern const FlipperExternalApplication FLIPPER_INTERNAL_EXTERNAL_APPS[];
extern const size_t FLIPPER_INTERNAL_EXTERNAL_APPS_COUNT;

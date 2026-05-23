#pragma once

#include <gui/view.h>
#include <lib/subghz/subghz_setting.h>
#include "../helpers/subghz_types.h"
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzJammer SubGhzJammer;

typedef void (*SubGhzJammerCallback)(SubGhzCustomEvent event, void* context);

void subghz_jammer_set_callback(
    SubGhzJammer* instance,
    SubGhzJammerCallback callback,
    void* context);

void subghz_jammer_set_setting(SubGhzJammer* instance, SubGhzSetting* setting);

SubGhzJammer* subghz_jammer_alloc(void);
void subghz_jammer_free(SubGhzJammer* instance);

View* subghz_jammer_get_view(SubGhzJammer* instance);

void subghz_jammer_start(SubGhzJammer* instance);
void subghz_jammer_stop(SubGhzJammer* instance);

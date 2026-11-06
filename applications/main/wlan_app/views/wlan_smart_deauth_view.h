#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <stdbool.h>
#include <stdint.h>

/* Model sebenarnya adalah SdViewModel (lihat ../wlan_smart_deauth.h). */

View* wlan_smart_deauth_view_alloc(void);
void  wlan_smart_deauth_view_free(View* view);

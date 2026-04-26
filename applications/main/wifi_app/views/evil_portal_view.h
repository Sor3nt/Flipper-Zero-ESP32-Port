#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct EvilPortalView EvilPortalView;

typedef void (*EvilPortalViewActionCb)(void* ctx);

EvilPortalView* evil_portal_view_alloc(void);
void evil_portal_view_free(EvilPortalView* v);
View* evil_portal_view_get_view(EvilPortalView* v);

void evil_portal_view_set_ssid(EvilPortalView* v, const char* ssid);
void evil_portal_view_set_channel(EvilPortalView* v, uint8_t channel);
void evil_portal_view_set_clients(EvilPortalView* v, uint16_t count);
void evil_portal_view_set_creds(EvilPortalView* v, uint32_t count);
void evil_portal_view_set_last(EvilPortalView* v, const char* user);
void evil_portal_view_set_status(EvilPortalView* v, const char* status);
/** Show / hide an animated hourglass with a status message overlay (used
 *  during the pre-scan and the verify phase). msg may be NULL when busy=false. */
void evil_portal_view_set_busy(EvilPortalView* v, bool busy, const char* msg);
void evil_portal_view_set_paused(EvilPortalView* v, bool paused);
void evil_portal_view_set_action_callback(
    EvilPortalView* v, EvilPortalViewActionCb cb, void* ctx);

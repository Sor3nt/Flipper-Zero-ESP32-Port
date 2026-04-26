#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct EvilPortalView EvilPortalView;

EvilPortalView* evil_portal_view_alloc(void);
void evil_portal_view_free(EvilPortalView* v);
View* evil_portal_view_get_view(EvilPortalView* v);

void evil_portal_view_set_ssid(EvilPortalView* v, const char* ssid);
void evil_portal_view_set_channel(EvilPortalView* v, uint8_t channel);
void evil_portal_view_set_clients(EvilPortalView* v, uint16_t count);
void evil_portal_view_set_creds(EvilPortalView* v, uint32_t count);
void evil_portal_view_set_last(EvilPortalView* v, const char* user);
void evil_portal_view_set_status(EvilPortalView* v, const char* status);

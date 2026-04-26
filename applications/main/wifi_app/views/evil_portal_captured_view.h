#pragma once
#include <gui/view.h>
#include <stdint.h>

typedef struct EvilPortalCapturedView EvilPortalCapturedView;

typedef void (*EvilPortalCapturedBackCb)(void* ctx);

EvilPortalCapturedView* evil_portal_captured_view_alloc(void);
void evil_portal_captured_view_free(EvilPortalCapturedView* v);
View* evil_portal_captured_view_get_view(EvilPortalCapturedView* v);

void evil_portal_captured_view_set_data(
    EvilPortalCapturedView* v, const char* ssid, const char* pwd);

void evil_portal_captured_view_set_back_callback(
    EvilPortalCapturedView* v, EvilPortalCapturedBackCb cb, void* ctx);

#pragma once

#include <gui/view.h>
#include "desktop_events.h"

typedef struct DesktopBruceConfirmView DesktopBruceConfirmView;

typedef void (*DesktopBruceConfirmViewCallback)(DesktopEvent event, void* context);

struct DesktopBruceConfirmView {
    View* view;
    DesktopBruceConfirmViewCallback callback;
    void* context;
};

void desktop_bruce_confirm_set_callback(
    DesktopBruceConfirmView* bruce_confirm,
    DesktopBruceConfirmViewCallback callback,
    void* context);

View* desktop_bruce_confirm_get_view(DesktopBruceConfirmView* bruce_confirm);
DesktopBruceConfirmView* desktop_bruce_confirm_alloc(void);
void desktop_bruce_confirm_free(DesktopBruceConfirmView* bruce_confirm);

#pragma once

#include "../wlan_app.h"

// Zentrales "Restoring devices..."-Overlay-Helper. Wird genutzt nach dem
// Disarm einer MITM/ARP-Spoof-Session, um der Scene Zeit zu geben für den
// Worker, ARP-Restore-Bursts an die Opfer zu schicken, bevor der View-Switch
// passiert. Tick-Decrement und Folge-Aktion (zurück zur Liste oder Scene-Pop)
// regelt die jeweilige Scene selbst.
#define WLAN_RESTORING_TICKS 12 // ~3 s bei 250 ms Tick-Rate

static inline void wlan_scene_show_restoring(
    WlanApp* app, const char* msg, uint16_t* ticks_out) {
    widget_reset(app->widget);
    widget_add_rect_element(app->widget, 14, 22, 100, 20, 3, false);
    widget_add_string_element(
        app->widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, msg);
    *ticks_out = WLAN_RESTORING_TICKS;
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

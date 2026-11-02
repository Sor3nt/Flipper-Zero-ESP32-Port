#include "../bw16_app_i.h"

void bw16_scene_about_on_enter(void* context) {
    Bw16App* app = context;
    Widget* widget = app->widget;

    widget_reset(widget);

    widget_add_string_element(
        widget, 64, 2, AlignCenter, AlignTop, FontPrimary, "R4TKN BW16 " BW16_APP_VERSION);

    widget_add_text_scroll_element(
        widget,
        0,
        14,
        128,
        50,
        "RTL8720DN (BW16) WiFi toolkit.\n"
        "Companion for the R4TKN BW16\n"
        "firmware by github.com/rusyln.\n"
        "\n"
        "Wiring (115200 baud):\n"
        "  BW16 5V  -> Flipper 1 (5V)\n"
        "  BW16 GND -> Flipper 8 (GND)\n"
        "  BW16 RX1 -> Flipper 13 (TX)\n"
        "  BW16 TX1 -> Flipper 14 (RX)\n"
        "\n"
        "Uses the shared expansion UART,\n"
        "so stop other UART apps first.\n"
        "5V is auto-enabled on battery.");

    view_dispatcher_switch_to_view(app->view_dispatcher, Bw16AppViewWidget);
}

bool bw16_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void bw16_scene_about_on_exit(void* context) {
    Bw16App* app = context;
    widget_reset(app->widget);
}

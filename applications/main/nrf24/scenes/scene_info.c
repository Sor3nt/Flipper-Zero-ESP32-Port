#include "../nrf24_app.h"

void nrf24_app_scene_info_on_enter(void* context) {
    Nrf24App* app = context;

    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignTop,
        "\e#NRF24L01 Toolkit\e#",
        false);
    widget_add_text_scroll_element(
        app->widget,
        0,
        16,
        128,
        48,
        "Built-in NRF24L01 on the\n"
        "T-Embed CC1101 board.\n"
        "Shares the SPI bus with\n"
        "CC1101 + LCD + SD.\n"
        "\n"
        "Spectrum: 2.400-2.479 GHz\n"
        "             80 channels (RPD)\n"
        "\n"
        "Jammer: 2 Mbps continuous wave\n"
        "             max PA power\n"
        "\n"
        "Educational use only -- do not\n"
        "harm networks you do not own.");

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
}

bool nrf24_app_scene_info_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nrf24_app_scene_info_on_exit(void* context) {
    Nrf24App* app = context;
    widget_reset(app->widget);
}

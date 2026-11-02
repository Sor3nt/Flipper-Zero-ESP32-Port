#include "../esp_flasher_app_i.h"

#if ESP_FLASHER_HAS_UART
#define ESP_FLASHER_ABOUT_WIRING                                                   \
    "\e#Wiring\n"                                                                  \
    "GPIO" ESP_FLASHER_STR(ESP_FLASHER_UART_TX) " (TX) -> target RX\n"             \
    "GPIO" ESP_FLASHER_STR(ESP_FLASHER_UART_RX) " (RX) <- target TX\n"             \
    "GND -> GND, target powered\n"
#else
#define ESP_FLASHER_ABOUT_WIRING "\e#Wiring\nThis board has no external UART.\n"
#endif

#if ESP_FLASHER_HAS_RESET_PINS
#define ESP_FLASHER_ABOUT_RESET                                                       \
    "Optional auto-reset:\n"                                                          \
    "GPIO" ESP_FLASHER_STR(ESP_FLASHER_EN_GPIO) " -> EN (RTS)\n"                      \
    "GPIO" ESP_FLASHER_STR(ESP_FLASHER_IO0_GPIO) " -> IO0 (DTR)\n"                    \
    "Settings > Reset pins:\n"                                                        \
    "esptool = active low (direct\nwiring or esptool circuit)\n"                      \
    "Flipper = active high\n"
#else
#define ESP_FLASHER_ABOUT_RESET "No reset pins: hold BOOT,\ntap RESET before flashing.\n"
#endif

#define ESP_FLASHER_APP_DESCRIPTION                                         \
    "\e#Information\nVersion: " ESP_FLASHER_APP_VERSION                     \
    "\nDeveloped by: 0xchocolate\n(@cococode on discord) using espressif's " \
    "esp-serial-flasher library\nGithub: "                                  \
    "https://github.com/0xchocolate/flipperzero-esp-flasher\n"             \
    "ESP32 port: UART, reset pins,\nC5 support\n\n"                          \
    "\e#Description\nApp to flash ESP chips over\nUART using bin files on "  \
    "the\nSD card. The chip is detected\nand the bootloader offset is\n"     \
    "picked for it (0x1000 ESP32/S2,\n0x0 S3/C3/C6, 0x2000 C5).\n\n"          \
    ESP_FLASHER_ABOUT_WIRING ESP_FLASHER_ABOUT_RESET                       \
    "\n\e#Quick Flash files\n/ext/apps_data/esp_flasher/\nassets/marauder/<chip>/\n"   \
    "(s2, wroom, s3, c5) and\nassets/custom/c5/:\nbootloader.bin\npartitions.bin\n"     \
    "boot_app0.bin\nfirmware.bin\nAddresses come from\npartitions.bin. Marauder C5\n"  \
    "needs 8 MB flash.\n\n"                                                            \
    "\e#Supported targets\n- ESP32\n- ESP8266\n- ESP32-S2\n- ESP32-S3\n"     \
    "- ESP32-C2\n- ESP32-C3\n- ESP32-C5\n- ESP32-C6\n- ESP32-H2"

void esp_flasher_scene_about_on_enter(void* context) {
    EspFlasherApp* app = context;

    widget_add_text_box_element(
        app->widget,
        0,
        0,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!                                                      \e!\n",
        false);
    widget_add_text_box_element(
        app->widget,
        0,
        2,
        128,
        14,
        AlignCenter,
        AlignBottom,
        "\e#\e!        ESP Flasher       \e!\n",
        false);
    widget_add_text_scroll_element(app->widget, 0, 16, 128, 50, ESP_FLASHER_APP_DESCRIPTION);

    view_dispatcher_switch_to_view(app->view_dispatcher, EspFlasherAppViewWidget);
}

bool esp_flasher_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void esp_flasher_scene_about_on_exit(void* context) {
    EspFlasherApp* app = context;

    // Clear views
    widget_reset(app->widget);
}

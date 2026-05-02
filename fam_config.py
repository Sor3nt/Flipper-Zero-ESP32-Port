import os

MANIFEST_ROOTS = [
    "components",
    "applications",
    "applications_user",
]

APP_SOURCE_OVERRIDES = {
    "desktop": "applications",
    "storage": "applications",
}

APPS = [
    "input",
    "notification",
    "gui",
    "dialogs",
    "locale",
    "cli",
    "cli_vcp",
    "storage",
    "storage_start",
    "power",
    "power_start",
    "power_settings",
    "loader",
    "loader_start",
    "notification_settings",
    "desktop",
    "archive",
    "about",
    "bt_settings",
    "example_apps_data",
    "example_apps_assets",
    "example_number_input",
    "clock",
    "bad_usb",
    "subghz",
    "cli_subghz",
    "subghz_load_dangerous_settings",
    "passport",
    "nfc",
    "infrared",
    "lfrfid",
    "wifi",
    "nrf24",
    "ble_spam",
    "js_app",
    "js_event_loop",
    "js_gui",
    "js_gui__loading",
    "js_gui__empty_screen",
    "js_gui__submenu",
    "js_gui__text_input",
    "js_gui__number_input",
    "js_gui__button_panel",
    "js_gui__popup",
    "js_gui__button_menu",
    "js_gui__menu",
    "js_gui__vi_list",
    "js_gui__byte_input",
    "js_gui__text_box",
    "js_gui__dialog",
    "js_gui__file_picker",
    "js_gui__widget",
    "js_gui__icon",
    "js_notification",
    "js_math",
    "js_storage",
    "js_subghz",
    "js_infrared",
    "js_blebeacon",
    # js_serial, js_gpio, js_i2c, js_spi excluded - need HAL porting
]

# Boards without NFC / IR hardware – exclude the corresponding apps
_board = os.environ.get("FLIPPER_BOARD", "")
_boards_without_nfc = {"waveshare_c6_1.9", "waveshare_c6_1.47"}
_boards_without_ir = {"waveshare_c6_1.9", "waveshare_c6_1.47"}

# Wolf3D shares Doom's requirements (PSRAM, ST7789 320xN, I2S speaker).
# Doom läuft ebenfalls nur auf T-Embed (PSRAM + 16 MB Flash) — wird aber als
# externer FAP gebaut (steht nicht in APPS), Block bleibt unten zur Klarheit.
_boards_without_wolf3d = {"waveshare_c6_1.9", "waveshare_c6_1.47"}

if _board in _boards_without_nfc:
    APPS = [a for a in APPS if a != "nfc"]

_boards_without_subghz = {"waveshare_c6_1.9", "waveshare_c6_1.47"}

# NRF24 plugs into the LORA slot (T-Embed CC1101). Boards without the slot
# don't have the required pin defines.
_boards_without_nrf24 = {"waveshare_c6_1.9", "waveshare_c6_1.47"}

if _board in _boards_without_ir:
    APPS = [a for a in APPS if a not in ("infrared", "js_infrared")]

if _board in _boards_without_subghz:
    APPS = [a for a in APPS if a not in ("subghz", "cli_subghz", "subghz_load_dangerous_settings", "js_subghz")]

if _board in _boards_without_nrf24:
    APPS = [a for a in APPS if a != "nrf24"]

if _board in _boards_without_wolf3d:
    APPS = [a for a in APPS if a != "wolf3d"]
# (wolf3d und doom stehen nicht in APPS — externer FAP-Pfad. Block bleibt für Klarheit.)

EXTRA_EXT_APPS = []
TARGET_HW = 32
AUTORUN_APP = ""

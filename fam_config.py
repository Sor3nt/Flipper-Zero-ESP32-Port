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
    "backlight_settings",
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
    "ble_spam",
    "tetris",
    "pocsag_pager",
]

# Boards without NFC / IR hardware – exclude the corresponding apps
_board = os.environ.get("FLIPPER_BOARD", "")
_boards_without_nfc = {"waveshare_c6_1.9", "waveshare_c6_1.47"}
_boards_without_ir = {"waveshare_c6_1.9", "waveshare_c6_1.47"}

if _board in _boards_without_nfc:
    APPS = [a for a in APPS if a != "nfc"]

if _board in _boards_without_ir:
    APPS = [a for a in APPS if a != "infrared"]

EXTRA_EXT_APPS = []
TARGET_HW = 32
AUTORUN_APP = ""

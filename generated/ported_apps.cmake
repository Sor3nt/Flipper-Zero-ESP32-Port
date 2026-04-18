set(ESP32_FAM_PORTED_OBJECT_TARGETS)

set(ESP32_FAM_ASSETS_SCRIPT "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/tools/fam/compile_icons.py")
set(ESP32_FAM_RUNTIME_ROOT "${ESP32_FAM_GENERATED_DIR}/fam_runtime_root")
set(ESP32_FAM_RUNTIME_EXT_ROOT "${ESP32_FAM_RUNTIME_ROOT}/ext")
set(ESP32_FAM_STAGE_ASSETS_STAMP "${ESP32_FAM_RUNTIME_ROOT}/.assets.stamp")

add_library(esp32_fam_app_cli OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/cli/cli_main_commands.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/cli/cli_main_shell.c"
)
target_include_directories(esp32_fam_app_cli PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/cli"
)
target_compile_definitions(esp32_fam_app_cli PRIVATE SRV_CLI)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli)

add_library(esp32_fam_app_cli_subghz OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_chat.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_cli.c"
)
target_include_directories(esp32_fam_app_cli_subghz PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli_subghz)

add_library(esp32_fam_app_example_apps_assets OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/example_apps_assets.c"
)
target_include_directories(esp32_fam_app_example_apps_assets PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_apps_assets)

add_library(esp32_fam_app_example_apps_data OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_data/example_apps_data.c"
)
target_include_directories(esp32_fam_app_example_apps_data PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_data"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_apps_data)

add_library(esp32_fam_app_example_number_input OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/example_number_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/scenes/example_number_input_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/scenes/example_number_input_scene_input_max.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/scenes/example_number_input_scene_input_min.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/scenes/example_number_input_scene_input_number.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input/scenes/example_number_input_scene_show_number.c"
)
target_include_directories(esp32_fam_app_example_number_input PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_number_input"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_number_input)

add_library(esp32_fam_app_js_blebeacon OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_blebeacon.c"
)
target_include_directories(esp32_fam_app_js_blebeacon PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_blebeacon PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_blebeacon)

add_library(esp32_fam_app_js_event_loop OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_event_loop/js_event_loop.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_event_loop/js_event_loop_api_table.cpp"
)
target_include_directories(esp32_fam_app_js_event_loop PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_event_loop PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_event_loop)

add_library(esp32_fam_app_js_gui OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/js_gui.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/js_gui_api_table.cpp"
)
target_include_directories(esp32_fam_app_js_gui PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui)

add_library(esp32_fam_app_js_gui__button_menu OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/button_menu.c"
)
target_include_directories(esp32_fam_app_js_gui__button_menu PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__button_menu PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__button_menu)

add_library(esp32_fam_app_js_gui__button_panel OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/button_panel.c"
)
target_include_directories(esp32_fam_app_js_gui__button_panel PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__button_panel PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__button_panel)

add_library(esp32_fam_app_js_gui__byte_input OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/byte_input.c"
)
target_include_directories(esp32_fam_app_js_gui__byte_input PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__byte_input PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__byte_input)

add_library(esp32_fam_app_js_gui__dialog OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/dialog.c"
)
target_include_directories(esp32_fam_app_js_gui__dialog PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__dialog PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__dialog)

add_library(esp32_fam_app_js_gui__empty_screen OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/empty_screen.c"
)
target_include_directories(esp32_fam_app_js_gui__empty_screen PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__empty_screen PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__empty_screen)

add_library(esp32_fam_app_js_gui__file_picker OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/file_picker.c"
)
target_include_directories(esp32_fam_app_js_gui__file_picker PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__file_picker PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__file_picker)

add_library(esp32_fam_app_js_gui__icon OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/icon.c"
)
target_include_directories(esp32_fam_app_js_gui__icon PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__icon PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__icon)

add_library(esp32_fam_app_js_gui__loading OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/loading.c"
)
target_include_directories(esp32_fam_app_js_gui__loading PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__loading PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__loading)

add_library(esp32_fam_app_js_gui__menu OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/menu.c"
)
target_include_directories(esp32_fam_app_js_gui__menu PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__menu PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__menu)

add_library(esp32_fam_app_js_gui__number_input OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/number_input.c"
)
target_include_directories(esp32_fam_app_js_gui__number_input PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__number_input PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__number_input)

add_library(esp32_fam_app_js_gui__popup OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/popup.c"
)
target_include_directories(esp32_fam_app_js_gui__popup PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__popup PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__popup)

add_library(esp32_fam_app_js_gui__submenu OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/submenu.c"
)
target_include_directories(esp32_fam_app_js_gui__submenu PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__submenu PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__submenu)

add_library(esp32_fam_app_js_gui__text_box OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/text_box.c"
)
target_include_directories(esp32_fam_app_js_gui__text_box PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__text_box PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__text_box)

add_library(esp32_fam_app_js_gui__text_input OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/text_input.c"
)
target_include_directories(esp32_fam_app_js_gui__text_input PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__text_input PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__text_input)

add_library(esp32_fam_app_js_gui__vi_list OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/vi_list.c"
)
target_include_directories(esp32_fam_app_js_gui__vi_list PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__vi_list PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__vi_list)

add_library(esp32_fam_app_js_gui__widget OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_gui/widget.c"
)
target_include_directories(esp32_fam_app_js_gui__widget PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_gui__widget PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_gui__widget)

add_library(esp32_fam_app_js_infrared OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_infrared/js_infrared.c"
)
target_include_directories(esp32_fam_app_js_infrared PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_infrared PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_infrared)

add_library(esp32_fam_app_js_math OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_math.c"
)
target_include_directories(esp32_fam_app_js_math PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_math PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_math)

add_library(esp32_fam_app_js_notification OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_notification.c"
)
target_include_directories(esp32_fam_app_js_notification PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_notification PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_notification)

add_library(esp32_fam_app_js_storage OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_storage.c"
)
target_include_directories(esp32_fam_app_js_storage PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_storage PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_storage)

add_library(esp32_fam_app_js_subghz OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_subghz/js_subghz.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_subghz/radio_device_loader.c"
)
target_include_directories(esp32_fam_app_js_subghz PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_subghz PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_subghz)

add_library(esp32_fam_app_subghz OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/radio_device_loader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_device.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_protocols.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_radio_device_loader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_worker.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_chat.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_gen_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_threshold_rssi.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_txrx.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_txrx_create_protocol_key.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_usb_export.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_load_file.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_load_select.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_run_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_save_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_save_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_setup_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_setup_extra.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_decode_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_jammer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_more_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_need_saving.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_playlist.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_radio_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_read_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver_config.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_rpc.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_save_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_save_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_saved.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_saved_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_button.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_counter.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_key.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_seed.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_serial.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_type.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_show_error.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_show_error_sub.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_signal_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_transmitter.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_cli.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_dangerous_freq.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_history.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_i.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_last_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/receiver.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subbrute_attack_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subbrute_main_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_frequency_analyzer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_jammer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_playlist.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_read_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/transmitter.c"
)
target_include_directories(esp32_fam_app_subghz PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz"
)
target_compile_definitions(esp32_fam_app_subghz PRIVATE APP_SUBGHZ)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_subghz)

add_library(esp32_fam_app_cli_vcp OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/cli/cli_vcp.c"
)
target_include_directories(esp32_fam_app_cli_vcp PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/cli"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli_vcp)

add_library(esp32_fam_app_js_app OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/js_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/js_modules.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/js_thread.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/js_value.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_flipper.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/modules/js_tests.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/plugin_api/app_api_table.cpp"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/views/console_view.c"
)
target_include_directories(esp32_fam_app_js_app PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_app PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_app)

add_library(esp32_fam_app_power_settings OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/power_settings_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene_battery_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene_power_off.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene_reboot.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene_reboot_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/scenes/power_settings_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app/views/battery_info.c"
)
target_include_directories(esp32_fam_app_power_settings PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/power_settings_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power_settings)

add_library(esp32_fam_app_storage_settings OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_benchmark.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_benchmark_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_factory_reset.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_format_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_formatting.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_internal_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_sd_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_unmount_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/scenes/storage_settings_scene_unmounted.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings/storage_settings.c"
)
target_include_directories(esp32_fam_app_storage_settings PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/storage_settings"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_storage_settings)

add_library(esp32_fam_app_nfc OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/felica_auth.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/mf_classic_key_cache.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/mf_ultralight_auth.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/mf_user_dict.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/mfkey32_logger.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/nfc_detected_protocols.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/nfc_emv_parser.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/nfc_supported_cards.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/emv/emv.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/emv/emv_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/felica/felica.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/felica/felica_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_3a/iso14443_3a.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_3a/iso14443_3a_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_3b/iso14443_3b.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_3b/iso14443_3b_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_4a/iso14443_4a.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_4a/iso14443_4a_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_4b/iso14443_4b.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso14443_4b/iso14443_4b_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso15693_3/iso15693_3.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/iso15693_3/iso15693_3_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_classic/mf_classic.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_classic/mf_classic_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_desfire/mf_desfire.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_desfire/mf_desfire_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_plus/mf_plus.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_plus/mf_plus_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_ultralight/mf_ultralight.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/mf_ultralight/mf_ultralight_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/nfc_protocol_support.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/nfc_protocol_support_gui_common.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/nfc_protocol_support_unlock_helper.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/ntag4xx/ntag4xx.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/ntag4xx/ntag4xx_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/slix/slix.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/slix/slix_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/st25tb/st25tb.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/st25tb/st25tb_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/type_4_tag/type_4_tag.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/protocol_support/type_4_tag/type_4_tag_render.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/helpers/slix_unlock.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/nfc_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_debug.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_delete_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_des_auth_key_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_des_auth_unlock_warn.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_detect.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_emulate.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_emv_transactions.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_exit_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_extra_actions.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_felica_more_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_felica_system.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_field.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_file_select.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_generate_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_detect_reader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_keys.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_add.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_list.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_warn_duplicate.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_mfkey_complete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_mfkey_nonces_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_show_keys.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial_wrong_card.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_desfire_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_desfire_more_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_dict_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_add.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_list.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_warn_duplicate.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_capture_pass.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_key_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_unlock_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_mf_ultralight_unlock_warn.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_more_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_read.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_read_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_read_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_restore_original.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_restore_original_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_retry_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_rpc.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_save_confirm.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_save_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_save_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_saved_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_select_protocol.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_set_atqa.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_set_sak.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_set_type.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_set_uid.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_slix_key_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_slix_unlock.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_slix_unlock_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_slix_unlock_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_supported_card.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/scenes/nfc_scene_write.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/views/detect_reader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/views/dict_attack.c"
)
target_include_directories(esp32_fam_app_nfc PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_nfc)

add_library(esp32_fam_app_infrared OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/infrared_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/infrared_remote.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/common/infrared_scene_universal_common.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_ask_back.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_ask_retry.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_debug.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_button_select.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_delete_done.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_move.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_rename.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_edit_rename_done.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_error_databases.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_gpio_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_learn.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_learn_done.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_learn_enter_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_learn_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_remote.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_remote_list.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_rpc.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_ac.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_audio.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_fan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_leds.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_projector.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/scenes/infrared_scene_universal_tv.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/views/infrared_debug_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/views/infrared_move_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/views/infrared_progress_view.c"
)
target_include_directories(esp32_fam_app_infrared PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_infrared)

add_library(esp32_fam_app_dolphin OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/dolphin/dolphin.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/dolphin/helpers/dolphin_deed.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/dolphin/helpers/dolphin_state.c"
)
target_include_directories(esp32_fam_app_dolphin PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/dolphin"
)
target_compile_definitions(esp32_fam_app_dolphin PRIVATE SRV_DOLPHIN)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_dolphin)

add_library(esp32_fam_app_power_start OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_cli.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/views/power_off.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/views/power_unplug_usb.c"
)
target_include_directories(esp32_fam_app_power_start PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power_start)

add_library(esp32_fam_app_wifi OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_airsnitch.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_airsnitch_scan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_ap_detail.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_channel_attack_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_spam_ssids_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_connect.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_crawler.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_crawler_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_deauther.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_handshake.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_handshake_channel.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_handshake_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_netscan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_password_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_portscan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_scanner.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_sniffer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scene_ssid_attack_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/scenes/scenes.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/airsnitch_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/ap_list.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/crawler_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/deauther_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/handshake_channel_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/handshake_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/netscan_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/views/sniffer_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_crawler.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_hal.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_handshake_parser.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_passwords.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app/wifi_pcap.c"
)
target_include_directories(esp32_fam_app_wifi PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/wifi_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_wifi)

add_library(esp32_fam_app_ble_spam OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/ble_spam_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/ble_spam_hal.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/ble_walk_hal.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_clone_active.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_clone_scan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_main.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_running.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_spam_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_walk_char_detail.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_walk_chars.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_walk_scan.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scene_walk_services.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/scenes/scenes.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/views/ble_spam_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/views/ble_walk_detail_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam/views/ble_walk_scan_view.c"
)
target_include_directories(esp32_fam_app_ble_spam PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/ble_spam"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_ble_spam)

add_library(esp32_fam_app_bad_usb OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/bad_usb_app.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/helpers/bad_usb_hid.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/helpers/ble_hid_ext_profile.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/helpers/ducky_script.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/helpers/ducky_script_commands.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/helpers/ducky_script_keycodes.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config_ble_mac.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config_ble_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config_layout.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config_usb_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_config_usb_vidpid.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_confirm_unpair.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_done.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_error.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_file_select.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/scenes/bad_usb_scene_work.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/views/bad_usb_view.c"
)
target_include_directories(esp32_fam_app_bad_usb PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_bad_usb)

add_library(esp32_fam_app_notification_settings OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/notification_settings/notification_settings_app.c"
)
target_include_directories(esp32_fam_app_notification_settings PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/notification_settings"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_notification_settings)

add_library(esp32_fam_app_passport OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/dolphin_passport/passport.c"
)
target_include_directories(esp32_fam_app_passport PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/settings/dolphin_passport"
)
target_compile_definitions(esp32_fam_app_passport PRIVATE APP_PASSPORT)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_passport)

add_library(esp32_fam_app_clock OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/clock_app.c"
)
target_include_directories(esp32_fam_app_clock PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_clock)

add_library(esp32_fam_app_js_app_start OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/js_app.c"
)
target_include_directories(esp32_fam_app_js_app_start PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app"
)
target_compile_options(esp32_fam_app_js_app_start PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_js_app_start)

add_library(esp32_fam_app_power OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_cli.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/power_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/views/power_off.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power/power_service/views/power_unplug_usb.c"
)
target_include_directories(esp32_fam_app_power PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/power"
)
target_compile_definitions(esp32_fam_app_power PRIVATE SRV_POWER)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power)

add_library(esp32_fam_app_storage OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/filesystem_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_cli.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_external_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_glue.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_internal_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_processing.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storage_sd_api.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storages/sd_notify.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage/storages/storage_ext.c"
)
target_include_directories(esp32_fam_app_storage PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/storage"
)
target_compile_definitions(esp32_fam_app_storage PRIVATE SRV_STORAGE)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_storage)

add_library(esp32_fam_app_desktop OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/animations/animation_manager.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/animations/animation_storage.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/animations/views/bubble_animation_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/animations/views/one_shot_animation_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/desktop.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/desktop_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/helpers/pin_code.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/helpers/slideshow.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_debug.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_fault.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_hw_mismatch.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_lock_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_locked.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_main.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_pin_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_pin_timeout.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_secure_enclave.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/scenes/desktop_scene_slideshow.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_debug.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_lock_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_locked.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_main.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_pin_input.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_pin_timeout.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop/views/desktop_view_slideshow.c"
)
target_include_directories(esp32_fam_app_desktop PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/services/desktop"
)
target_compile_definitions(esp32_fam_app_desktop PRIVATE SRV_DESKTOP)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_desktop)

add_library(esp32_fam_app_subghz_load_dangerous_settings OBJECT
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/radio_device_loader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_device.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_protocols.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_radio_device_loader.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subbrute_worker.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_chat.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_gen_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_threshold_rssi.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_txrx.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_txrx_create_protocol_key.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/helpers/subghz_usb_export.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_load_file.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_load_select.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_run_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_save_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_save_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_setup_attack.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_setup_extra.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_bf_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_decode_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_delete_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_jammer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_more_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_need_saving.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_playlist.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_radio_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_read_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver_config.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_receiver_info.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_rpc.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_save_name.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_save_success.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_saved.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_saved_menu.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_button.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_counter.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_key.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_seed.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_serial.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_set_type.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_show_error.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_show_error_sub.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_signal_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_start.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/scenes/subghz_scene_transmitter.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_cli.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_dangerous_freq.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_history.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_i.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/subghz_last_settings.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/receiver.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subbrute_attack_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subbrute_main_view.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_frequency_analyzer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_jammer.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_playlist.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/subghz_read_raw.c"
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/views/transmitter.c"
)
target_include_directories(esp32_fam_app_subghz_load_dangerous_settings PRIVATE
    "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_subghz_load_dangerous_settings)

add_custom_command(
    OUTPUT "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ESP32_FAM_RUNTIME_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/example_apps_assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/files" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/example_apps_assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/subghz"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/subghz"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/js_app"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/js_app"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/nfc"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/nfc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/infrared"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/infrared"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/bad_usb"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/bad_usb"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/clock"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/clock"
    COMMAND ${CMAKE_COMMAND} -E touch "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    DEPENDS
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/files/poems/a jelly-fish.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/files/poems/my shadow.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/files/poems/theme in yellow.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/examples/example_apps_assets/files/test_asset.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/ba-BA.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/colemak.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/cz_CS.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/da-DA.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/de-CH.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/de-DE-mac.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/de-DE.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/dvorak.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/en-UK.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/en-US.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/es-ES.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/es-LA.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fi-FI.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fr-BE.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fr-CA.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fr-CH.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fr-FR-mac.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/fr-FR.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/hr-HR.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/hu-HU.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/it-IT-mac.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/it-IT.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/nb-NO.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/nl-NL.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/pt-BR.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/pt-PT.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/si-SI.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/sk-SK.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/sv-SE.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/assets/layouts/tr-TR.kl"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/demo_chromeos.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/demo_gnome.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/demo_macos.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/demo_windows.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/Install_qFlipper_gnome.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/Install_qFlipper_macOS.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/Install_qFlipper_windows.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/bad_usb/resources/badusb/test_mouse.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/ibtnfuzzer/example_uids_cyfral.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/ibtnfuzzer/example_uids_ds1990.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/ibtnfuzzer/example_uids_metakom.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/music_player/Marble_Machine.fmf"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/rfidfuzzer/example_uids_em4100.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/rfidfuzzer/example_uids_h10301.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/rfidfuzzer/example_uids_hidprox.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/rfidfuzzer/example_uids_pac.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/subplaylist/example_playlist.txt"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/100us.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/call_test_1.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/call_test_2.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/dump_0x00000000_1k.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/dump_0x00000000_4b.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/dump_STM32.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/goto_test.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/halt.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/reset.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/clock_app/resources/swd_scripts/test_write.swd"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/ac.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/audio.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/fans.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/leds.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/projectors.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/infrared/resources/infrared/assets/tv.ir"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/aid.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/country_code.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/currency_code.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/mf_classic_dict.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/mf_ultralight_c_dict.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/skylanders.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/nfc/resources/nfc/assets/vendors.nfc"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/alutech_at_4n"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/dangerous_settings"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user.example"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/nice_flor_s"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/main/subghz/resources/subghz/assets/setting_user.example"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/array_buf_test.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/bad_uart.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/badusb_demo.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/blebeacon.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/console.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/delay.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/event_loop.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/gpio.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/gui.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/i2c.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/infrared-send.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/interactive.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/load.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/load_api.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/math.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/notify.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/path.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/spi.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/storage.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/stringutils.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/subghz.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/uart_echo.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/uart_echo_8e1.js"
        "C:/Users/U_Know_69/Downloads/Flipper-Zero-ESP32-Port-main/Flipper-Zero-ESP32-Port-main/applications/system/js_app/examples/apps/Scripts/js_examples/usbdisk.js"
    VERBATIM
)
add_custom_target(esp32_fam_stage_assets DEPENDS "${ESP32_FAM_STAGE_ASSETS_STAMP}")

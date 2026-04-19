set(ESP32_FAM_PORTED_OBJECT_TARGETS)

set(ESP32_FAM_ASSETS_SCRIPT "/Users/matthias/www/privat/flipper-zero-esp32-port_git/tools/fam/compile_icons.py")
set(ESP32_FAM_RUNTIME_ROOT "${ESP32_FAM_GENERATED_DIR}/fam_runtime_root")
set(ESP32_FAM_RUNTIME_EXT_ROOT "${ESP32_FAM_RUNTIME_ROOT}/ext")
set(ESP32_FAM_STAGE_ASSETS_STAMP "${ESP32_FAM_RUNTIME_ROOT}/.assets.stamp")

add_library(esp32_fam_app_cli OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/cli/cli_main_commands.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/cli/cli_main_shell.c"
)
target_include_directories(esp32_fam_app_cli PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/cli"
)
target_compile_definitions(esp32_fam_app_cli PRIVATE SRV_CLI)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli)

add_library(esp32_fam_app_cli_subghz OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_chat.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_cli.c"
)
target_include_directories(esp32_fam_app_cli_subghz PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli_subghz)

add_library(esp32_fam_app_example_apps_assets OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/example_apps_assets.c"
)
target_include_directories(esp32_fam_app_example_apps_assets PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_apps_assets)

add_library(esp32_fam_app_example_apps_data OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_data/example_apps_data.c"
)
target_include_directories(esp32_fam_app_example_apps_data PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_data"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_apps_data)

add_library(esp32_fam_app_example_number_input OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/example_number_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/scenes/example_number_input_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/scenes/example_number_input_scene_input_max.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/scenes/example_number_input_scene_input_min.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/scenes/example_number_input_scene_input_number.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input/scenes/example_number_input_scene_show_number.c"
)
target_include_directories(esp32_fam_app_example_number_input PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_number_input"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_example_number_input)

add_library(esp32_fam_app_subghz OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/radio_device_loader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_device.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_protocols.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_radio_device_loader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_worker.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_chat.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_gen_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_threshold_rssi.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_txrx.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_txrx_create_protocol_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_usb_export.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_load_file.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_load_select.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_run_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_setup_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_setup_extra.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_decode_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_jammer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_more_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_need_saving.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_radio_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_read_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver_config.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_rpc.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_saved.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_saved_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_button.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_counter.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_seed.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_serial.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_type.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_show_error.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_show_error_sub.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_signal_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_transmitter.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_dangerous_freq.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_history.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_i.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_last_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subbrute_attack_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subbrute_main_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_frequency_analyzer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_jammer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_read_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/transmitter.c"
)
target_include_directories(esp32_fam_app_subghz PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz"
)
target_compile_definitions(esp32_fam_app_subghz PRIVATE APP_SUBGHZ)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_subghz)

add_library(esp32_fam_app_backlight_settings OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/backlight_settings/backlight_settings_app.c"
)
target_include_directories(esp32_fam_app_backlight_settings PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/backlight_settings"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_backlight_settings)

add_library(esp32_fam_app_cli_vcp OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/cli/cli_vcp.c"
)
target_include_directories(esp32_fam_app_cli_vcp PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/cli"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_cli_vcp)

add_library(esp32_fam_app_power_settings OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/power_settings_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene_battery_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene_power_off.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene_reboot.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene_reboot_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/scenes/power_settings_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app/views/battery_info.c"
)
target_include_directories(esp32_fam_app_power_settings PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/power_settings_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power_settings)

add_library(esp32_fam_app_lfrfid OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/lfrfid.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/lfrfid_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_clear_t5577.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_clear_t5577_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_delete_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_delete_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_emulate.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_enter_password.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_exit_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_extra_actions.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_raw_emulate.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_raw_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_raw_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_raw_read.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_raw_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_read.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_read_key_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_read_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_retry_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_rpc.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_save_data.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_save_type.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_saved_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_saved_key_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_select_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_select_raw_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_write.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_write_and_set_pass.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/scenes/lfrfid_scene_write_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/views/lfrfid_view_read.c"
)
target_include_directories(esp32_fam_app_lfrfid PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_lfrfid)

add_library(esp32_fam_app_storage_settings OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_benchmark.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_benchmark_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_factory_reset.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_format_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_formatting.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_internal_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_sd_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_unmount_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/scenes/storage_settings_scene_unmounted.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings/storage_settings.c"
)
target_include_directories(esp32_fam_app_storage_settings PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/storage_settings"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_storage_settings)

add_custom_command(
    OUTPUT "${ESP32_FAM_GENERATED_DIR}/icons/tetris/tetris_icons.c" "${ESP32_FAM_GENERATED_DIR}/icons/tetris/tetris_icons.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_GENERATED_DIR}/icons/tetris"
    COMMAND ${Python3_EXECUTABLE} ${ESP32_FAM_ASSETS_SCRIPT} icons "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game/assets" "${ESP32_FAM_GENERATED_DIR}/icons/tetris" --filename "tetris_icons"
    DEPENDS
        ${ESP32_FAM_ASSETS_SCRIPT}
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game/assets/Pin_back_arrow_10x8.png"
    VERBATIM
)

add_library(esp32_fam_app_tetris OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game/tetris_game.c"
    "${ESP32_FAM_GENERATED_DIR}/icons/tetris/tetris_icons.c"
)
target_include_directories(esp32_fam_app_tetris PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game"
    "${ESP32_FAM_GENERATED_DIR}/icons/tetris"
)
target_compile_definitions(esp32_fam_app_tetris PRIVATE FAP_VERSION="2.6")
target_compile_options(esp32_fam_app_tetris PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_tetris)

add_library(esp32_fam_app_nfc OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/felica_auth.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/mf_classic_key_cache.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/mf_ultralight_auth.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/mf_user_dict.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/mfkey32_logger.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/nfc_detected_protocols.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/nfc_emv_parser.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/nfc_supported_cards.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/emv/emv.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/emv/emv_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/felica/felica.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/felica/felica_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_3a/iso14443_3a.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_3a/iso14443_3a_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_3b/iso14443_3b.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_3b/iso14443_3b_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_4a/iso14443_4a.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_4a/iso14443_4a_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_4b/iso14443_4b.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso14443_4b/iso14443_4b_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso15693_3/iso15693_3.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/iso15693_3/iso15693_3_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_classic/mf_classic.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_classic/mf_classic_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_desfire/mf_desfire.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_desfire/mf_desfire_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_plus/mf_plus.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_plus/mf_plus_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_ultralight/mf_ultralight.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/mf_ultralight/mf_ultralight_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/nfc_protocol_support.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/nfc_protocol_support_gui_common.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/nfc_protocol_support_unlock_helper.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/ntag4xx/ntag4xx.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/ntag4xx/ntag4xx_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/slix/slix.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/slix/slix_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/st25tb/st25tb.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/st25tb/st25tb_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/type_4_tag/type_4_tag.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/protocol_support/type_4_tag/type_4_tag_render.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/helpers/slix_unlock.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/nfc_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_debug.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_delete_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_des_auth_key_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_des_auth_unlock_warn.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_detect.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_emulate.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_emv_transactions.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_exit_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_extra_actions.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_felica_more_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_felica_system.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_field.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_file_select.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_generate_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_detect_reader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_keys.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_add.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_list.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_keys_warn_duplicate.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_mfkey_complete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_mfkey_nonces_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_show_keys.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_classic_update_initial_wrong_card.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_desfire_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_desfire_more_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_dict_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_add.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_list.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_c_keys_warn_duplicate.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_capture_pass.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_key_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_unlock_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_mf_ultralight_unlock_warn.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_more_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_read.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_read_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_read_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_restore_original.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_restore_original_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_retry_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_rpc.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_save_confirm.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_saved_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_select_protocol.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_set_atqa.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_set_sak.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_set_type.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_set_uid.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_slix_key_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_slix_unlock.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_slix_unlock_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_slix_unlock_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_supported_card.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/scenes/nfc_scene_write.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/views/detect_reader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/views/dict_attack.c"
)
target_include_directories(esp32_fam_app_nfc PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_nfc)

add_library(esp32_fam_app_infrared OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/infrared_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/infrared_remote.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/common/infrared_scene_universal_common.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_ask_back.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_ask_retry.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_debug.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_button_select.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_delete_done.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_move.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_rename.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_edit_rename_done.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_error_databases.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_gpio_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_learn.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_learn_done.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_learn_enter_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_learn_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_remote.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_remote_list.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_rpc.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_ac.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_audio.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_fan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_leds.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_projector.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/scenes/infrared_scene_universal_tv.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/views/infrared_debug_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/views/infrared_move_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/views/infrared_progress_view.c"
)
target_include_directories(esp32_fam_app_infrared PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_infrared)

add_library(esp32_fam_app_dolphin OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/dolphin/dolphin.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/dolphin/helpers/dolphin_deed.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/dolphin/helpers/dolphin_state.c"
)
target_include_directories(esp32_fam_app_dolphin PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/dolphin"
)
target_compile_definitions(esp32_fam_app_dolphin PRIVATE SRV_DOLPHIN)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_dolphin)

add_custom_command(
    OUTPUT "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager/pocsag_pager_icons.c" "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager/pocsag_pager_icons.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager"
    COMMAND ${Python3_EXECUTABLE} ${ESP32_FAM_ASSETS_SCRIPT} icons "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images" "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager" --filename "pocsag_pager_icons"
    DEPENDS
        ${ESP32_FAM_ASSETS_SCRIPT}
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Fishing_123x52.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Lock_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Message_8x7.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Pin_back_arrow_10x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Quest_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Scanning_123x52.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Unlock_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/WarningDolphin_45x42.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/pocsag_pager_10px.png"
    VERBATIM
)

add_library(esp32_fam_app_pocsag_pager OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/helpers/radio_device_loader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/pocsag_pager_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/pocsag_pager_app_i.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/pocsag_pager_history.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/protocols/pcsg_generic.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/protocols/pocsag.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/protocols/protocol_items.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_scene_about.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_scene_receiver_config.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_scene_receiver_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/scenes/pocsag_pager_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/views/pocsag_pager_receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/views/pocsag_pager_receiver_info.c"
    "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager/pocsag_pager_icons.c"
)
target_include_directories(esp32_fam_app_pocsag_pager PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager"
    "${ESP32_FAM_GENERATED_DIR}/icons/pocsag_pager"
)
target_compile_definitions(esp32_fam_app_pocsag_pager PRIVATE FAP_VERSION="1.4")
target_compile_options(esp32_fam_app_pocsag_pager PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_pocsag_pager)

add_library(esp32_fam_app_power_start OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/views/power_off.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/views/power_unplug_usb.c"
)
target_include_directories(esp32_fam_app_power_start PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power_start)

add_library(esp32_fam_app_wifi OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_airsnitch.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_airsnitch_scan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_ap_detail.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_channel_attack_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_connect.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_crawler.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_crawler_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_deauther.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_handshake.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_handshake_channel.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_handshake_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_netscan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_password_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_portscan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_scanner.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_sniffer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scene_ssid_attack_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/scenes/scenes.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/airsnitch_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/ap_list.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/crawler_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/deauther_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/handshake_channel_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/handshake_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/netscan_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/views/sniffer_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_crawler.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_hal.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_handshake_parser.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_passwords.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app/wifi_pcap.c"
)
target_include_directories(esp32_fam_app_wifi PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/wifi_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_wifi)

add_library(esp32_fam_app_ble_spam OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/ble_spam_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/ble_spam_hal.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/ble_walk_hal.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_clone_active.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_clone_scan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_main.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_running.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_spam_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_walk_char_detail.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_walk_chars.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_walk_scan.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scene_walk_services.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/scenes/scenes.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/views/ble_spam_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/views/ble_walk_detail_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam/views/ble_walk_scan_view.c"
)
target_include_directories(esp32_fam_app_ble_spam PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/ble_spam"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_ble_spam)

add_library(esp32_fam_app_bad_usb OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/bad_usb_app.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/helpers/bad_usb_hid.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/helpers/ble_hid_ext_profile.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/helpers/ducky_script.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/helpers/ducky_script_commands.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/helpers/ducky_script_keycodes.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config_ble_mac.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config_ble_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config_layout.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config_usb_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_config_usb_vidpid.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_confirm_unpair.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_done.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_error.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_file_select.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/scenes/bad_usb_scene_work.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/views/bad_usb_view.c"
)
target_include_directories(esp32_fam_app_bad_usb PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_bad_usb)

add_library(esp32_fam_app_passport OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/dolphin_passport/passport.c"
)
target_include_directories(esp32_fam_app_passport PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/settings/dolphin_passport"
)
target_compile_definitions(esp32_fam_app_passport PRIVATE APP_PASSPORT)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_passport)

add_library(esp32_fam_app_clock OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/clock_app.c"
)
target_include_directories(esp32_fam_app_clock PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_clock)

add_library(esp32_fam_app_power OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/power_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/views/power_off.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power/power_service/views/power_unplug_usb.c"
)
target_include_directories(esp32_fam_app_power PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/power"
)
target_compile_definitions(esp32_fam_app_power PRIVATE SRV_POWER)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_power)

add_library(esp32_fam_app_storage OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/filesystem_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_external_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_glue.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_internal_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_processing.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storage_sd_api.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storages/sd_notify.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage/storages/storage_ext.c"
)
target_include_directories(esp32_fam_app_storage PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/storage"
)
target_compile_definitions(esp32_fam_app_storage PRIVATE SRV_STORAGE)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_storage)

add_library(esp32_fam_app_desktop OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/animations/animation_manager.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/animations/animation_storage.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/animations/views/bubble_animation_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/animations/views/one_shot_animation_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/desktop.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/desktop_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/helpers/pin_code.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/helpers/slideshow.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_debug.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_fault.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_hw_mismatch.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_lock_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_locked.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_main.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_pin_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_pin_timeout.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_secure_enclave.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/scenes/desktop_scene_slideshow.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_debug.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_lock_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_locked.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_main.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_pin_input.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_pin_timeout.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop/views/desktop_view_slideshow.c"
)
target_include_directories(esp32_fam_app_desktop PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/services/desktop"
)
target_compile_definitions(esp32_fam_app_desktop PRIVATE SRV_DESKTOP)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_desktop)

add_library(esp32_fam_app_subghz_load_dangerous_settings OBJECT
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/radio_device_loader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_device.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_protocols.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_radio_device_loader.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subbrute_worker.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_chat.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_gen_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_threshold_rssi.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_txrx.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_txrx_create_protocol_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/helpers/subghz_usb_export.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_load_file.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_load_select.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_run_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_setup_attack.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_setup_extra.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_bf_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_decode_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_delete_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_jammer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_more_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_need_saving.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_radio_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_read_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver_config.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_receiver_info.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_rpc.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_save_name.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_save_success.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_saved.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_saved_menu.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_button.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_counter.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_key.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_seed.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_serial.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_set_type.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_show_error.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_show_error_sub.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_signal_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_start.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/scenes/subghz_scene_transmitter.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_cli.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_dangerous_freq.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_history.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_i.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/subghz_last_settings.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/receiver.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subbrute_attack_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subbrute_main_view.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_frequency_analyzer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_jammer.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/subghz_read_raw.c"
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/views/transmitter.c"
)
target_include_directories(esp32_fam_app_subghz_load_dangerous_settings PRIVATE
    "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz"
)
list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS esp32_fam_app_subghz_load_dangerous_settings)

add_custom_command(
    OUTPUT "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ESP32_FAM_RUNTIME_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/example_apps_assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/files" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/example_apps_assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/subghz"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/subghz"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/lfrfid"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/lfrfid"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/tetris"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game/assets" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/tetris"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/nfc"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/nfc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/infrared"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/infrared"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/pocsag_pager"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/pocsag_pager"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/bad_usb"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/bad_usb"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/clock"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources" "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets/clock"
    COMMAND ${CMAKE_COMMAND} -E touch "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    DEPENDS
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/files/poems/a jelly-fish.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/files/poems/my shadow.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/files/poems/theme in yellow.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/examples/example_apps_assets/files/test_asset.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/Install_qFlipper_gnome.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/Install_qFlipper_macOS.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/Install_qFlipper_windows.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/ba-BA.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/colemak.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/cz_CS.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/da-DA.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/de-CH.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/de-DE-mac.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/de-DE.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/dvorak.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/en-UK.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/en-US.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/es-ES.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/es-LA.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fi-FI.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fr-BE.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fr-CA.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fr-CH.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fr-FR-mac.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/fr-FR.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/hr-HR.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/hu-HU.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/it-IT-mac.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/it-IT.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/nb-NO.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/nl-NL.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/pt-BR.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/pt-PT.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/si-SI.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/sk-SK.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/sv-SE.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/assets/layouts/tr-TR.kl"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/demo_chromeos.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/demo_gnome.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/demo_macos.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/demo_windows.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/bad_usb/resources/badusb/test_mouse.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/ibtnfuzzer/example_uids_cyfral.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/ibtnfuzzer/example_uids_ds1990.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/ibtnfuzzer/example_uids_metakom.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/music_player/Marble_Machine.fmf"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/rfidfuzzer/example_uids_em4100.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/rfidfuzzer/example_uids_h10301.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/rfidfuzzer/example_uids_hidprox.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/rfidfuzzer/example_uids_pac.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/subplaylist/example_playlist.txt"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/100us.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/call_test_1.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/call_test_2.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/dump_0x00000000_1k.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/dump_0x00000000_4b.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/dump_STM32.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/goto_test.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/halt.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/reset.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/clock_app/resources/swd_scripts/test_write.swd"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/ac.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/audio.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/fans.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/leds.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/projectors.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/infrared/resources/infrared/assets/tv.ir"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/lfrfid/resources/lfrfid/assets/iso3166.lfrfid"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/aid.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/country_code.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/currency_code.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/mf_classic_dict.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/mf_ultralight_c_dict.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/skylanders.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/nfc/resources/nfc/assets/vendors.nfc"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/alutech_at_4n"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/dangerous_settings"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/keeloq_mfcodes_user.example"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/nice_flor_s"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications/main/subghz/resources/subghz/assets/setting_user.example"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Fishing_123x52.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Lock_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Message_8x7.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Pin_back_arrow_10x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Quest_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Scanning_123x52.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/Unlock_7x8.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/WarningDolphin_45x42.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/pocsag_pager/images/pocsag_pager_10px.png"
        "/Users/matthias/www/privat/flipper-zero-esp32-port_git/applications_user/tetris_game/assets/Pin_back_arrow_10x8.png"
    VERBATIM
)
add_custom_target(esp32_fam_stage_assets DEPENDS "${ESP32_FAM_STAGE_ASSETS_STAMP}")

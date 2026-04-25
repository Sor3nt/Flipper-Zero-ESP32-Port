set(ESP32_FAM_PORTED_OBJECT_TARGETS)

set(ESP32_FAM_ASSETS_SCRIPT "C:\\Dev\\Flipper-Zero-ESP32-Port-main\\Flipper-Zero-ESP32-Port-main\\tools\\fam\\compile_icons.py")
set(ESP32_FAM_RUNTIME_ROOT "${ESP32_FAM_GENERATED_DIR}/fam_runtime_root")
set(ESP32_FAM_RUNTIME_EXT_ROOT "${ESP32_FAM_RUNTIME_ROOT}/ext")
set(ESP32_FAM_STAGE_ASSETS_STAMP "${ESP32_FAM_RUNTIME_ROOT}/.assets.stamp")

add_custom_command(
    OUTPUT "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ESP32_FAM_RUNTIME_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets"
    COMMAND ${CMAKE_COMMAND} -E touch "${ESP32_FAM_STAGE_ASSETS_STAMP}"
    VERBATIM
)
add_custom_target(esp32_fam_stage_assets DEPENDS "${ESP32_FAM_STAGE_ASSETS_STAMP}")

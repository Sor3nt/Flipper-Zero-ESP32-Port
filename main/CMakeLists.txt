find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(ESP32_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(ESP32_FAM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ESP32_FAM_REGISTRY_C "${ESP32_FAM_GENERATED_DIR}/applications.c")
set(ESP32_FAM_PORTED_CMAKE "${ESP32_FAM_GENERATED_DIR}/ported_apps.cmake")

if(NOT DEFINED FLIPPER_BOARD)
    set(FLIPPER_BOARD "waveshare_c6_1.9")
endif()

set(ENV{FLIPPER_BOARD} "${FLIPPER_BOARD}")

# Re-run CMake configure when FAM inputs change so execute_process() regenerates
# ported_apps.cmake (not allowed in ESP-IDF script-mode includes; see CMAKE_SCRIPT_MODE_FILE).
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB _esp32_fam_dep1 CONFIGURE_DEPENDS "${ESP32_DIR}/fam_config.py")
    file(GLOB _esp32_fam_dep2 CONFIGURE_DEPENDS "${ESP32_DIR}/tools/fam/generate.py")
    file(GLOB _esp32_fam_dep3 CONFIGURE_DEPENDS "${ESP32_DIR}/tools/fam/appmanifest.py")
    file(GLOB _esp32_fam_m1 CONFIGURE_DEPENDS "${ESP32_DIR}/applications/*/application.fam")
    file(GLOB _esp32_fam_m2 CONFIGURE_DEPENDS "${ESP32_DIR}/applications/*/*/application.fam")
    file(GLOB _esp32_fam_m3 CONFIGURE_DEPENDS "${ESP32_DIR}/applications/*/*/*/application.fam")
    file(GLOB _esp32_fam_m4 CONFIGURE_DEPENDS "${ESP32_DIR}/applications/*/*/*/*/application.fam")
    file(GLOB _esp32_fam_m5 CONFIGURE_DEPENDS "${ESP32_DIR}/applications/*/*/*/*/*/application.fam")
    file(GLOB _esp32_fam_u1 CONFIGURE_DEPENDS "${ESP32_DIR}/applications_user/*/application.fam")
    file(GLOB _esp32_fam_u2 CONFIGURE_DEPENDS "${ESP32_DIR}/applications_user/*/*/application.fam")
    file(GLOB _esp32_fam_c1 CONFIGURE_DEPENDS "${ESP32_DIR}/components/*/application.fam")
    file(GLOB _esp32_fam_c2 CONFIGURE_DEPENDS "${ESP32_DIR}/components/*/*/application.fam")
    set(_ESP32_FAM_CONFIGURE_DEPENDS
        ${_esp32_fam_dep1} ${_esp32_fam_dep2} ${_esp32_fam_dep3}
        ${_esp32_fam_m1} ${_esp32_fam_m2} ${_esp32_fam_m3} ${_esp32_fam_m4} ${_esp32_fam_m5}
        ${_esp32_fam_u1} ${_esp32_fam_u2}
        ${_esp32_fam_c1} ${_esp32_fam_c2})
endif()

execute_process(
    COMMAND
        ${Python3_EXECUTABLE}
        "${ESP32_DIR}/tools/fam/generate.py"
        --project-dir "${ESP32_DIR}"
        --config "${ESP32_DIR}/fam_config.py"
        --registry-out "${ESP32_FAM_REGISTRY_C}"
        --cmake-out "${ESP32_FAM_PORTED_CMAKE}"
    RESULT_VARIABLE ESP32_FAM_GENERATE_RESULT
    OUTPUT_VARIABLE ESP32_FAM_GENERATE_STDOUT
    ERROR_VARIABLE ESP32_FAM_GENERATE_STDERR
)

if(NOT ESP32_FAM_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "ESP32 FAM generation failed:\n${ESP32_FAM_GENERATE_STDOUT}\n${ESP32_FAM_GENERATE_STDERR}")
endif()

idf_component_register(
    SRCS "app_main.c" "${ESP32_FAM_REGISTRY_C}"
    INCLUDE_DIRS "."
    REQUIRES furi furi_hal input notification gui locale storage loader desktop btshim
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    include("${ESP32_FAM_PORTED_CMAKE}")

    add_dependencies(${COMPONENT_LIB} esp32_fam_stage_assets)

    set(ESP32_FAM_PORTED_COMPONENT_LIBS
        idf::archive
        idf::assets
        idf::ble_hid
        idf::ble_profile
        idf::btshim
        idf::datetime
        idf::desktop
        idf::dialogs
        idf::flipper_application
        idf::furi
        idf::furi_hal
        idf::gui
        idf::input
        idf::loader
        idf::locale
        idf::notification
        idf::storage
        idf::nfc
        idf::bit_lib
        idf::infrared
        idf::subghz
        idf::toolbox
        idf::esp_http_client
        idf::mjs
        idf::momentum
    )

    foreach(esp32_fam_target IN LISTS ESP32_FAM_PORTED_OBJECT_TARGETS)
        target_link_libraries(${esp32_fam_target} PRIVATE ${ESP32_FAM_PORTED_COMPONENT_LIBS})
        target_include_directories(${esp32_fam_target} PRIVATE
            "${ESP32_FAM_GENERATED_DIR}"
            "${ESP32_DIR}/components"
            "${ESP32_DIR}/lib"
            "${ESP32_DIR}/lib/subghz"
            "${ESP32_DIR}/lib/infrared"
            "${ESP32_DIR}/lib/infrared/encoder_decoder"
            "${ESP32_DIR}/lib/infrared/signal"
            "${ESP32_DIR}/lib/infrared/worker"
            "${ESP32_DIR}/lib/mjs"
            "${ESP32_DIR}/lib/mjs/common"
            "${ESP32_DIR}"
            "${ESP32_DIR}/applications"
            "${ESP32_DIR}/applications/main"
            "${ESP32_DIR}/applications/services"
            "${ESP32_DIR}/applications/settings"
            "${ESP32_DIR}/applications/system"
            "${ESP32_DIR}/applications/drivers"
            "${ESP32_DIR}/applications/debug"
        )
        target_sources(${COMPONENT_LIB} PRIVATE $<TARGET_OBJECTS:${esp32_fam_target}>)
    endforeach()
endif()

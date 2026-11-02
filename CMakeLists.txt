cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# Keep the hardware profile aligned with the selected ESP target when the
# caller has not explicitly selected a board. ESP32-S3 defaults to Smoochie;
# C6 retains the existing Waveshare default. Other S3 boards can still be
# selected explicitly with -DFLIPPER_BOARD=...
if(NOT DEFINED FLIPPER_BOARD)
    if(DEFINED ENV{FLIPPER_BOARD})
        set(FLIPPER_BOARD "$ENV{FLIPPER_BOARD}" CACHE STRING "Target board")
    else()
        set(_configured_idf_target "$ENV{IDF_TARGET}")
        if(NOT _configured_idf_target AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
            file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig" _idf_target_line
                REGEX "^CONFIG_IDF_TARGET=\"")
            string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"([^\"]+)\"$" "\\1"
                _configured_idf_target "${_idf_target_line}")
        endif()
        if(_configured_idf_target STREQUAL "esp32s3")
            set(FLIPPER_BOARD "smoochie" CACHE STRING "Target board")
        else()
            set(FLIPPER_BOARD "waveshare_c6_1.9" CACHE STRING "Target board")
        endif()
    endif()
endif()
set(ENV{FLIPPER_BOARD} "${FLIPPER_BOARD}")

# Paradoxt shares GPIO43/44 with UART1, so keep its console on USB Serial/JTAG.
if(FLIPPER_BOARD STREQUAL "paradoxt")
    if(NOT DEFINED SDKCONFIG_DEFAULTS OR SDKCONFIG_DEFAULTS STREQUAL "")
        set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
    endif()
    list(FIND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.paradoxt"
        _paradoxt_defaults_index)
    if(_paradoxt_defaults_index EQUAL -1)
        list(APPEND SDKCONFIG_DEFAULTS
            "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.paradoxt")
    endif()
endif()

# ---------------------------------------------------------------------------
# Patch libnet80211.a to allow raw TX of deauth/disassoc management frames.
# The closed-source WiFi blob rejects frame types 0xC0/0xA0; we patch
# ieee80211_raw_frame_sanity_check() to return 0. Idempotent — skips if
# already patched.  Runs at configure time (before library path resolution).
# ---------------------------------------------------------------------------
set(_wifi_lib "$ENV{IDF_PATH}/components/esp_wifi/lib/esp32s3/libnet80211.a")
find_program(_xt_ar NAMES xtensa-esp32s3-elf-ar
    PATHS "$ENV{IDF_TOOLS_PATH}/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin"
          "$ENV{USERPROFILE}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin"
          "$ENV{HOME}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin")
find_program(_xt_objcopy NAMES xtensa-esp32s3-elf-objcopy
    PATHS "$ENV{IDF_TOOLS_PATH}/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin"
          "$ENV{USERPROFILE}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin"
          "$ENV{HOME}/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin")

if(_xt_ar AND _xt_objcopy AND EXISTS "${_wifi_lib}")
    find_program(PYTHON_EXECUTABLE NAMES python3 python)
    if(PYTHON_EXECUTABLE)
        execute_process(
            COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/patch_wifi_lib.py"
                "${_wifi_lib}" "${_xt_objcopy}" "${_xt_ar}" "${_wifi_lib}"
            RESULT_VARIABLE _patch_rc OUTPUT_VARIABLE _patch_out ERROR_VARIABLE _patch_err)
        if(_patch_rc EQUAL 0)
            message(STATUS "${_patch_out}")
        else()
            message(WARNING "WiFi lib patch failed: ${_patch_err}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# App-Descriptor-Version (esp_app_desc_t.version) aus der zentralen Port-Version
# in components/toolbox/fw_version.h. Damit traegt jede furi_esp32.bin ihre
# Versionsnummer im Image-Header (Offset 0x30) — qT-Embed liest sie vor dem
# Flashen, der OTA-Updater schreibt sie nach dem Flashen in /ext/.fw_version.
# Ohne PROJECT_VER wuerde ESP-IDF "git describe" eintragen.
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/components/toolbox/fw_version.h" _furi_ver_line
    REGEX "^#define[ \t]+FURI_ESP32_VERSION[ \t]+\"")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_furi_ver_line}")
if(NOT PROJECT_VER)
    message(FATAL_ERROR "FURI_ESP32_VERSION not found in components/toolbox/fw_version.h")
endif()
message(STATUS "Firmware version (PROJECT_VER): ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(furi_esp32)

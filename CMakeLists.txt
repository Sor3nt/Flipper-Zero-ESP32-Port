cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

# Keep board-specific sdkconfig values isolated from the shared workspace
# sdkconfig. Paradoxt and T-Embed use the shared reboot helper, which needs the
# dual-core and ESP Timer ISR settings from their board defaults. Explicit
# per-build SDKCONFIG paths are preserved; board defaults are appended if needed.
if(NOT DEFINED FLIPPER_BOARD AND DEFINED ENV{FLIPPER_BOARD})
    set(FLIPPER_BOARD "$ENV{FLIPPER_BOARD}")
endif()

if(("${FLIPPER_BOARD}" STREQUAL "paradoxt" OR
    "${FLIPPER_BOARD}" STREQUAL "lilygo_t_embed_cc1101") AND
   EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${FLIPPER_BOARD}")
        if(DEFINED SDKCONFIG AND NOT "${SDKCONFIG}" STREQUAL "")
            get_filename_component(_requested_sdkconfig "${SDKCONFIG}" ABSOLUTE
                BASE_DIR "${CMAKE_SOURCE_DIR}")
        else()
            set(_requested_sdkconfig "")
        endif()
        get_filename_component(_shared_sdkconfig "${CMAKE_SOURCE_DIR}/sdkconfig" ABSOLUTE)

        # If this build directory was previously configured by this block for
        # another board, switch to a separate config file instead of reusing
        # that board's generated sdkconfig. A user-supplied path remains intact.
        if(NOT _requested_sdkconfig OR
           "${_requested_sdkconfig}" STREQUAL "${_shared_sdkconfig}" OR
           (DEFINED FURI_AUTO_SDKCONFIG AND
            "${_requested_sdkconfig}" STREQUAL "${FURI_AUTO_SDKCONFIG}"))
            set(_managed_sdkconfig "${CMAKE_BINARY_DIR}/sdkconfig.${FLIPPER_BOARD}")
            set(SDKCONFIG "${_managed_sdkconfig}" CACHE FILEPATH
                "Board-specific ESP-IDF sdkconfig" FORCE)
            set(FURI_AUTO_SDKCONFIG "${_managed_sdkconfig}" CACHE INTERNAL
                "SDKCONFIG path managed by the selected Flipper board" FORCE)
        endif()

        # Keep the original defaults separate from defaults auto-appended by a
        # previous configure, so changing boards never loads both board files.
        if(DEFINED FURI_AUTO_SDKCONFIG_DEFAULTS AND
           "${SDKCONFIG_DEFAULTS}" STREQUAL "${FURI_AUTO_SDKCONFIG_DEFAULTS}")
            set(_base_sdkconfig_defaults "${FURI_BASE_SDKCONFIG_DEFAULTS}")
        elseif(NOT DEFINED SDKCONFIG_DEFAULTS OR "${SDKCONFIG_DEFAULTS}" STREQUAL "")
            set(_base_sdkconfig_defaults "sdkconfig.defaults")
        else()
            set(_base_sdkconfig_defaults "${SDKCONFIG_DEFAULTS}")
        endif()

        set(_board_sdkconfig_defaults_list "${_base_sdkconfig_defaults}")
        list(FIND _board_sdkconfig_defaults_list "sdkconfig.defaults.${FLIPPER_BOARD}"
            _board_defaults_index)
        if(_board_defaults_index EQUAL -1)
            list(APPEND _board_sdkconfig_defaults_list "sdkconfig.defaults.${FLIPPER_BOARD}")
        endif()

        set(SDKCONFIG_DEFAULTS "${_board_sdkconfig_defaults_list}" CACHE STRING
            "ESP-IDF sdkconfig defaults for the selected board" FORCE)
        set(FURI_BASE_SDKCONFIG_DEFAULTS "${_base_sdkconfig_defaults}" CACHE INTERNAL
            "SDKCONFIG_DEFAULTS before board-specific defaults were appended" FORCE)
        set(FURI_AUTO_SDKCONFIG_DEFAULTS "${_board_sdkconfig_defaults_list}" CACHE INTERNAL
            "SDKCONFIG_DEFAULTS managed by the selected Flipper board" FORCE)
    else()
        # Restore the shared project config/defaults when switching this build
        # directory back to a board without an isolated defaults file.
        get_filename_component(_shared_sdkconfig "${CMAKE_SOURCE_DIR}/sdkconfig" ABSOLUTE)
        if(DEFINED SDKCONFIG AND DEFINED FURI_AUTO_SDKCONFIG AND
           NOT "${FURI_AUTO_SDKCONFIG}" STREQUAL "")
            get_filename_component(_requested_sdkconfig "${SDKCONFIG}" ABSOLUTE
                BASE_DIR "${CMAKE_SOURCE_DIR}")
            if("${_requested_sdkconfig}" STREQUAL "${FURI_AUTO_SDKCONFIG}")
                set(SDKCONFIG "${_shared_sdkconfig}" CACHE FILEPATH
                    "ESP-IDF sdkconfig for the selected board" FORCE)
                unset(FURI_AUTO_SDKCONFIG CACHE)
            endif()
        endif()

        if(DEFINED SDKCONFIG_DEFAULTS AND DEFINED FURI_AUTO_SDKCONFIG_DEFAULTS AND
           "${SDKCONFIG_DEFAULTS}" STREQUAL "${FURI_AUTO_SDKCONFIG_DEFAULTS}")
            set(SDKCONFIG_DEFAULTS "${FURI_BASE_SDKCONFIG_DEFAULTS}" CACHE STRING
                "ESP-IDF sdkconfig defaults without board-specific additions" FORCE)
            unset(FURI_AUTO_SDKCONFIG_DEFAULTS CACHE)
            unset(FURI_BASE_SDKCONFIG_DEFAULTS CACHE)
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
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/components/toolbox/fw_version.h")
file(STRINGS "${CMAKE_SOURCE_DIR}/components/toolbox/fw_version.h" _furi_ver_line
    REGEX "^#define[ \t]+FURI_ESP32_VERSION[ \t]+\"")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_furi_ver_line}")
if(NOT PROJECT_VER)
    message(FATAL_ERROR "FURI_ESP32_VERSION not found in components/toolbox/fw_version.h")
endif()
message(STATUS "Firmware version (PROJECT_VER): ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(furi_esp32)
include(cmake/wifite_sdk_adapter.cmake)

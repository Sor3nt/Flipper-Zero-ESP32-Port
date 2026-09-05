cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "components")

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

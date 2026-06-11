#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

BOARD="waveshare_c6_1.9"
# This is the established build dir for the 1.9 board (also used by build.sh).
BUILD_DIR="build_waveshare_c6"
IDF_TARGET="esp32c6"

# The generic sdkconfig.defaults is already tailored to the 1.9 board (8MB
# flash, partitions_singleapp_large.csv), so unlike the 1.47 there is no
# board-specific override file — only append one if it actually exists.
# Keep sdkconfig inside the build dir so this board doesn't clobber the
# git-tracked root sdkconfig (which the T-Embed/esp32s3 build uses).
SDKCONFIG_DEFAULTS="sdkconfig.defaults"
if [[ -f "${ESP32_DIR}/sdkconfig.defaults.${BOARD}" ]]; then
    SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.${BOARD}"
fi
SDKCONFIG="${BUILD_DIR}/sdkconfig"

# Waveshare C6 exposes a native USB-Serial-JTAG port; some units also enumerate
# via a UART bridge. Search the usual suspects on macOS and Linux.
detect_usbmodem_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/ttyACM* /dev/ttyUSB*)
    shopt -u nullglob

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    fi

    if [[ "${#matches[@]}" -eq 0 ]]; then
        if [[ "${BUILD_ONLY}" -eq 0 ]]; then
            echo "No serial device found (searched /dev/cu.usbmodem*, /dev/cu.usbserial*, /dev/ttyACM*, /dev/ttyUSB*). Use --port or set ESPPORT." >&2
            return 1
        else
            return 0
        fi
    else
        echo "Multiple serial devices found: ${matches[*]}" >&2
        echo "Use --port or set ESPPORT." >&2
        return 1
    fi
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only]

Builds and flashes this ESP32 Flipper Zero port for the Waveshare
ESP32-C6-LCD-1.9 board (single-app, 8MB flash, no multi-boot).

Options:
  --port <device>  Serial device to flash. Default: auto-detect /dev/cu.usbmodem* (macOS) or /dev/ttyACM*/ttyUSB* (Linux)
  --monitor        Open idf.py monitor after flashing
  --build-only     Build, skip flashing

Environment:
  ESPPORT                  Overrides the auto-detected serial device
  ESP_IDF_EXPORT_SCRIPT    Overrides the ESP-IDF export.sh path
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port|-p)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for $1" >&2
                usage
                exit 1
            fi
            PORT="$2"
            shift 2
            ;;
        --monitor|-m)
            RUN_MONITOR=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_usbmodem_port)"
fi

if [[ ! -f "${EXPORT_SCRIPT}" ]]; then
    echo "ESP-IDF export script not found: ${EXPORT_SCRIPT}" >&2
    exit 1
fi

echo "Board:          ${BOARD}"
echo "Target:         ${IDF_TARGET}"
echo "Build dir:      ${BUILD_DIR}"
echo "sdkconfig:      ${SDKCONFIG}"
echo "Using ESP-IDF:  ${EXPORT_SCRIPT}"
echo "Serial port:    ${PORT}"

cd "${ESP32_DIR}"

# Kill any process holding the serial port exclusively (e.g. a left-over
# `idf.py monitor`, `screen`, `pyserial`). Without this the flash fails with
# "Could not exclusively lock port [...] Resource temporarily unavailable".
release_serial_port() {
    local port="$1"
    [[ -z "${port}" || ! -e "${port}" ]] && return 0
    if ! command -v lsof >/dev/null 2>&1; then return 0; fi
    local pids
    pids="$(lsof -t "${port}" 2>/dev/null || true)"
    if [[ -n "${pids}" ]]; then
        echo "Releasing serial port ${port} from PID(s): ${pids}" >&2
        # shellcheck disable=SC2086
        kill -9 ${pids} 2>/dev/null || true
        sleep 0.3
    fi
}

echo
echo "=== Building this firmware (${BOARD}) ==="

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    release_serial_port "${PORT}"
fi

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

cd "${ESP32_DIR}"

# Set target if build dir doesn't exist yet or the cached target differs.
NEED_SET_TARGET=0
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    NEED_SET_TARGET=1
elif ! grep -q "IDF_TARGET:STRING=${IDF_TARGET}" "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null; then
    echo "Cached target in ${BUILD_DIR} differs from ${IDF_TARGET}; re-running set-target."
    NEED_SET_TARGET=1
fi

if [[ "${NEED_SET_TARGET}" -eq 1 ]]; then
    echo "Setting IDF target to ${IDF_TARGET}..."
    idf.py -B "${BUILD_DIR}" \
        -DSDKCONFIG="${SDKCONFIG}" \
        -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
        set-target "${IDF_TARGET}"
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py -B "${BUILD_DIR}" \
        -DFLIPPER_BOARD="${BOARD}" \
        -DSDKCONFIG="${SDKCONFIG}" \
        -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
        reconfigure build
    echo
    echo "Build complete (--build-only). Nothing flashed."
    exit 0
fi

idf.py -B "${BUILD_DIR}" \
    -DFLIPPER_BOARD="${BOARD}" \
    -DSDKCONFIG="${SDKCONFIG}" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS}" \
    -p "${PORT}" \
    reconfigure build flash

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    release_serial_port "${PORT}"
    idf.py -B "${BUILD_DIR}" -p "${PORT}" monitor
fi

#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-/Users/matthias/esp/esp-idf/export.sh}"

BOARD="lilygo_t_embed_cc1101"
BUILD_DIR="build_t_embed"
IDF_TARGET="esp32s3"

detect_usbmodem_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem*)
    shopt -u nullglob

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    fi

    if [[ "${#matches[@]}" -eq 0 ]]; then
        if [[ "${BUILD_ONLY}" -eq 0 ]]; then
            echo "No /dev/cu.usbmodem* device found. Use --port or set ESPPORT." >&2
            return 1
        else 
            return 0
        fi
    else
        echo "Multiple /dev/cu.usbmodem* devices found: ${matches[*]}" >&2
        echo "Use --port or set ESPPORT." >&2
        return 1
    fi
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only]

Builds and flashes the ESP32-S3 firmware for LilyGo T-Embed CC1101.

Options:
  --port <device>  Serial device to flash. Default: auto-detect /dev/cu.usbmodem*
  --monitor        Open idf.py monitor after flashing
  --build-only     Build only, skip flashing

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
echo "Using ESP-IDF:  ${EXPORT_SCRIPT}"
echo "Serial port:    ${PORT}"

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

cd "${ESP32_DIR}"

# Set target if build dir doesn't exist yet or target changed
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "Setting IDF target to ${IDF_TARGET}..."
    idf.py -B "${BUILD_DIR}" set-target "${IDF_TARGET}"
fi

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    if [[ "${BUILD_ONLY}" -eq 1 ]]; then
        idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" reconfigure build
        exit 0
    fi
    idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" -p "${PORT}" reconfigure build flash monitor
else
    if [[ "${BUILD_ONLY}" -eq 1 ]]; then
        idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" reconfigure build
    else
        idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" -p "${PORT}" reconfigure build flash
    fi
fi

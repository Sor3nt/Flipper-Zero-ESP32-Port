#!/usr/bin/env bash
# Build (and optionally flash) the DIY board: ESP32-S3 with push-button input.
# Pins are configured in Pinout.txt.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"
BOARD="DIY"
BUILD_DIR="build_diy"
IDF_TARGET="esp32s3"
PORT="${ESPPORT:-}"
BUILD_ONLY=0
MONITOR=0
CLEAN=0

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only] [--clean]

Builds and flashes the Flipper Zero ESP32 port for the DIY board.
Edit Pinout.txt to change the GPIO assignment.

Options:
  -p, --port <device>  Serial device (default: auto-detect /dev/ttyACM* or /dev/cu.usbmodem*)
  -m, --monitor        Open the serial monitor after flashing
      --build-only     Build only, do not flash
      --clean          Remove ${BUILD_DIR} before building

Environment:
  ESPPORT                Overrides the serial device
  ESP_IDF_EXPORT_SCRIPT  Path to ESP-IDF export.sh (default: ~/esp/esp-idf/export.sh)
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 1; }; PORT="$2"; shift 2 ;;
        -m|--monitor) MONITOR=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ ! -f "${EXPORT_SCRIPT}" ]]; then
    echo "ESP-IDF export script not found: ${EXPORT_SCRIPT}" >&2
    echo "Set ESP_IDF_EXPORT_SCRIPT to its location." >&2
    exit 1
fi

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    shopt -s nullglob
    ports=(/dev/ttyACM* /dev/cu.usbmodem*)
    shopt -u nullglob
    if [[ "${#ports[@]}" -ne 1 ]]; then
        echo "Expected exactly one serial device, found ${#ports[@]}. Use --port or ESPPORT." >&2
        exit 1
    fi
    PORT="${ports[0]}"
fi

cd "${SCRIPT_DIR}"
# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

[[ "${CLEAN}" -eq 1 ]] && rm -rf "${BUILD_DIR}"

echo "Board:     ${BOARD} (${IDF_TARGET})"
echo "Pinout:    ${SCRIPT_DIR}/Pinout.txt"
echo "Build dir: ${BUILD_DIR}"
[[ "${BUILD_ONLY}" -eq 0 ]] && echo "Port:      ${PORT}"

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    idf.py -B "${BUILD_DIR}" set-target "${IDF_TARGET}"
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" reconfigure build
    echo "Build complete (--build-only), nothing flashed."
    exit 0
fi

idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" -p "${PORT}" reconfigure build flash
[[ "${MONITOR}" -eq 1 ]] && idf.py -B "${BUILD_DIR}" -p "${PORT}" monitor

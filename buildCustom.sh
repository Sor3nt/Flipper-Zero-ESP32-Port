#!/usr/bin/env bash
set -euo pipefail

# Build script for custom ESP32-S3 board

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

# Configuration for custom board
BOARD="reaper_fury"
BUILD_DIR="build_custom"
TARGET="esp32s3"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]
Options:
  -p|--port       Serial port (auto-detected if not specified)
  -m|--monitor    Run monitor after flash
  --build-only    Only build, don't flash
  -h|--help       Show this help
EOF
}

detect_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB*)
    shopt -u nullglob
    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
    elif [[ "${#matches[@]}" -gt 1 ]]; then
        echo "Multiple ports found: ${matches[*]}. Specify with --port." >&2
        return 1
    else
        [[ "${BUILD_ONLY}" -eq 0 ]] && echo "No device found." >&2
        return 1
    fi
}

release_port() {
    local port="$1"
    if [[ -n "${port}" ]]; then
        pids="$(lsof -t "${port}" 2>/dev/null || true)"
        [[ -n "${pids}" ]] && kill -9 ${pids} && sleep 0.3
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)    PORT="$2"; shift 2 ;;
        -m|--monitor) RUN_MONITOR=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Unknown: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_port || echo "")"
    [[ -z "${PORT}" ]] && exit 1
fi

[[ ! -f "${EXPORT_SCRIPT}" ]] && echo "IDF export script missing at ${EXPORT_SCRIPT}" >&2 && exit 1

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}" 2>/dev/null || {
    echo "Error: Failed to source ESP-IDF environment"
    echo "Make sure ESP-IDF is installed at: ${EXPORT_SCRIPT}"
    exit 1
}

cd "${SCRIPT_DIR}"

echo "=== Building ${BOARD} for ${TARGET} ==="
echo "Build directory: ${BUILD_DIR}"
echo "Board: ${BOARD}"

[[ "${BUILD_ONLY}" -eq 0 ]] && release_port "${PORT}"

# Build
idf.py -DFLIPPER_BOARD="${BOARD}" -B "${BUILD_DIR}" build

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    echo ""
    echo "=== Flashing to ${PORT} ==="
    idf.py -DFLIPPER_BOARD="${BOARD}" -B "${BUILD_DIR}" -p "${PORT}" flash
    
    if [[ "${RUN_MONITOR}" -eq 1 ]]; then
        echo ""
        echo "=== Monitoring ${PORT} ==="
        idf.py -DFLIPPER_BOARD="${BOARD}" -B "${BUILD_DIR}" -p "${PORT}" monitor
    fi
else
    echo ""
    echo "=== Build complete ==="
    echo "To flash: idf.py -DFLIPPER_BOARD=${BOARD} -B ${BUILD_DIR} -p /dev/ttyXXX flash"
    echo "To monitor: idf.py -DFLIPPER_BOARD=${BOARD} -B ${BUILD_DIR} -p /dev/ttyXXX monitor"
fi

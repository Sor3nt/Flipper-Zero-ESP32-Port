#!/usr/bin/env bash
# Build (and flash) the ESP32 buddy firmware.
#
# Usage:
#   ./build.sh                       # target esp32, build + flash (auto port)
#   ./build.sh esp32s3               # build + flash for ESP32-S3
#   ./build.sh esp32c6 /dev/ttyACM0  # explicit target + port
#   ./build.sh esp32 --build-only    # build, no flash
#
# Each target gets its own build dir (build_<target>) so you can keep several
# in parallel. ESP-IDF is expected at $HOME/esp/esp-idf (override via
# ESP_IDF_EXPORT_SCRIPT), same as the master firmware.
set -euo pipefail

TARGET="${1:-esp32}"
ARG2="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

: "${ESP_IDF_EXPORT_SCRIPT:=$HOME/esp/esp-idf/export.sh}"
if [ ! -f "$ESP_IDF_EXPORT_SCRIPT" ]; then
    echo "ESP-IDF export script not found: $ESP_IDF_EXPORT_SCRIPT" >&2
    echo "Set ESP_IDF_EXPORT_SCRIPT to your esp-idf/export.sh" >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$ESP_IDF_EXPORT_SCRIPT"

BUILD_DIR="build_${TARGET}"

BUILD_ONLY=0
PORT=""
case "$ARG2" in
    --build-only) BUILD_ONLY=1 ;;
    "") ;;
    *) PORT="$ARG2" ;;
esac

PORT_ARGS=()
[ -n "$PORT" ] && PORT_ARGS=(-p "$PORT")

# set-target only on a fresh build dir (it wipes sdkconfig otherwise).
if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    idf.py -B "$BUILD_DIR" set-target "$TARGET"
fi

# Note: ${arr[@]+"${arr[@]}"} expands safely even for an empty array under the
# `set -u` of macOS' bash 3.2 (a plain "${arr[@]}" would error there).
idf.py -B "$BUILD_DIR" ${PORT_ARGS[@]+"${PORT_ARGS[@]}"} build

if [ "$BUILD_ONLY" -eq 0 ]; then
    idf.py -B "$BUILD_DIR" ${PORT_ARGS[@]+"${PORT_ARGS[@]}"} flash
    echo
    echo "Flashed. Monitor with:  idf.py -B ${BUILD_DIR} ${PORT:+-p $PORT }monitor"
fi

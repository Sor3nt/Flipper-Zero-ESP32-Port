#!/usr/bin/env bash

set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

# Default State
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
SELECTED_BOARD=""

# Hardware Definitions
declare -A TARGETS=( ["s3"]="esp32s3" ["c6"]="esp32c6" ["t_embed"]="esp32s3" )
declare -A NAMES=( ["s3"]="esp32s3_generic" ["c6"]="waveshare_c6_1.9" ["t_embed"]="lilygo_t_embed_cc1101" )
declare -A DIRS=( ["s3"]="build_s3" ["c6"]="build_waveshare_c6" ["t_embed"]="build_t_embed" )

usage() {
    cat <<EOF
Usage: $(basename "$0") --board <name> [options]
Boards: s3, c6, t_embed
Options: -p|--port, -m|--monitor, --build-only
EOF
}

detect_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM*)
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
    [[ -z "${port}" || ! -e "${port}" ]] && return 0
    if command -v lsof >/dev/null 2>&1; then
        local pids
        pids="$(lsof -t "${port}" 2>/dev/null || true)"
        [[ -n "${pids}" ]] && kill -9 ${pids} && sleep 0.3
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--board) SELECTED_BOARD="$2"; shift 2 ;;
        -p|--port)  PORT="$2"; shift 2 ;;
        -m|--monitor) RUN_MONITOR=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "${SELECTED_BOARD}" || -z "${NAMES[$SELECTED_BOARD]+x}" ]]; then
    echo "Error: Valid --board required (s3, c6, t_embed)." >&2
    exit 1
fi

BOARD="${NAMES[$SELECTED_BOARD]}"
BUILD_DIR="${DIRS[$SELECTED_BOARD]}"
TARGET="${TARGETS[$SELECTED_BOARD]}"

if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_port || echo "")"
    [[ -z "${PORT}" ]] && exit 1
fi

[[ ! -f "${EXPORT_SCRIPT}" ]] && echo "IDF export script missing." >&2 && exit 1

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"
cd "${SCRIPT_DIR}"

[[ "${BUILD_ONLY}" -eq 0 ]] && release_port "${PORT}"

# Sync sdkconfig with current chip target
idf.py -B "${BUILD_DIR}" set-target "${TARGET}"

# Construct Command
COMMANDS=("reconfigure" "build")
PY_OPTS=("-B" "${BUILD_DIR}" "-DFLIPPER_BOARD=${BOARD}")

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    COMMANDS+=("flash")
    PY_OPTS+=("-p" "${PORT}")
    [[ "${RUN_MONITOR}" -eq 1 ]] && COMMANDS+=("monitor")
fi

idf.py "${PY_OPTS[@]}" "${COMMANDS[@]}"
#!/usr/bin/env bash

set -euo pipefail

# --- Configuration & Board Definitions ---
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

# Default values
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
SELECTED_BOARD=""

# Define supported boards
declare -A TARGET_MAP=( ["s3"]="esp32s3" ["c6"]="esp32c6" ["t_embed"]="esp32s3" )
declare -A BOARD_NAME_MAP=( ["s3"]="esp32s3_generic" ["c6"]="waveshare_c6_1.9" ["t_embed"]="lilygo_t_embed_cc1101" )
declare -A BUILD_DIR_MAP=( ["s3"]="build_s3" ["c6"]="build_waveshare_c6" ["t_embed"]="build_t_embed" )

usage() {
    cat <<EOF
Usage: $(basename "$0") --board <name> [options]

Required:
  -b, --board <name>       Board to use: s3, c6, t_embed

Options:
  -p, --port <device>      Serial port. Default: auto-detect
  -m, --monitor            Open idf.py monitor after flashing
  --build-only             Build only, skip flashing/monitoring
  -h, --help               Show this help

Examples:
  ./$(basename "$0") -b c6 --monitor
  ./$(basename "$0") --board t_embed --build-only
EOF
}

detect_usbmodem_port() {
    local matches=()
    shopt -s nullglob
    # Combined search patterns from all three original scripts
    matches=(/dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM*)
    shopt -u nullglob

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    elif [[ "${#matches[@]}" -eq 0 ]]; then
        [[ "${BUILD_ONLY}" -eq 0 ]] && echo "No serial device found. Use --port." >&2
        return 1
    else
        echo "Multiple devices found: ${matches[*]}. Use --port." >&2
        return 1
    fi
}

release_serial_port() {
    local port="$1"
    [[ -z "${port}" || ! -e "${port}" ]] && return 0
    if command -v lsof >/dev/null 2>&1; then
        local pids
        pids="$(lsof -t "${port}" 2>/dev/null || true)"
        if [[ -n "${pids}" ]]; then
            echo "Releasing port ${port} from PID(s): ${pids}" >&2
            kill -9 ${pids} 2>/dev/null || true
            sleep 0.3
        fi
    fi
}

# --- Parse Arguments ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--board) SELECTED_BOARD="$2"; shift 2 ;;
        -p|--port)  PORT="$2"; shift 2 ;;
        -m|--monitor) RUN_MONITOR=1; shift ;;
        --build-only) BUILD_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

# Validate Board Selection
if [[ -z "${SELECTED_BOARD}" ]]; then
    echo "Error: No board specified." >&2
    usage; exit 1
fi

if [[ -z "${BOARD_NAME_MAP[$SELECTED_BOARD]+x}" ]]; then
    echo "Error: Invalid board '$SELECTED_BOARD'. Valid options: s3, c6, t_embed" >&2
    exit 1
fi

# Map variables based on selection
BOARD="${BOARD_NAME_MAP[$SELECTED_BOARD]}"
BUILD_DIR="${BUILD_DIR_MAP[$SELECTED_BOARD]}"
IDF_TARGET="${TARGET_MAP[$SELECTED_BOARD]}"

# Logic for Port Detection
if [[ -z "${PORT}" && "${BUILD_ONLY}" -eq 0 ]]; then
    PORT="$(detect_usbmodem_port || echo "")"
    if [[ -z "${PORT}" ]]; then exit 1; fi
fi

if [[ ! -f "${EXPORT_SCRIPT}" ]]; then
    echo "ESP-IDF export script not found: ${EXPORT_SCRIPT}" >&2
    exit 1
fi

# --- Execution ---
echo "--- Build Configuration ---"
echo "Board:      ${BOARD}"
echo "Target:     ${IDF_TARGET}"
echo "Build Dir:  ${BUILD_DIR}"
[[ "${BUILD_ONLY}" -eq 0 ]] && echo "Port:       ${PORT}"
echo "--------------------------"

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    release_serial_port "${PORT}"
fi

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"
cd "${ESP32_DIR}"

# Initialize target if needed
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "Initializing build directory for ${IDF_TARGET}..."
    idf.py -B "${BUILD_DIR}" set-target "${IDF_TARGET}"
fi

# Prepare idf.py commands
COMMANDS=("reconfigure" "build")
if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    COMMANDS+=("flash")
    [[ "${RUN_MONITOR}" -eq 1 ]] && COMMANDS+=("monitor")
fi

# Build arguments array
PY_OPTS=("-B" "${BUILD_DIR}" "-DFLIPPER_BOARD=${BOARD}")
[[ "${BUILD_ONLY}" -eq 0 ]] && PY_OPTS+=("-p" "${PORT}")

# Execute
idf.py "${PY_OPTS[@]}" "${COMMANDS[@]}"
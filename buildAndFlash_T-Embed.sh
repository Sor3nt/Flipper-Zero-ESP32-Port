#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

BOARD="lilygo_t_embed_cc1101"
BUILD_DIR="build_t_embed"
IDF_TARGET="esp32s3"

detect_usbmodem_port() {
    local matches=()
    shopt -s nullglob
    matches=(/dev/cu.usbmodem* /dev/ttyACM*)
    shopt -u nullglob

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    fi

    if [[ "${#matches[@]}" -eq 0 ]]; then
        if [[ "${BUILD_ONLY}" -eq 0 ]]; then
            echo "No serial device found (searched /dev/cu.usbmodem* and /dev/ttyACM*). Use --port or set ESPPORT." >&2
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

Builds and flashes the ESP32 Flipper Zero port for the LilyGo T-Embed CC1101.

Options:
  --port <device>  Serial device to flash. Default: auto-detect /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux)
  --monitor        Open idf.py monitor after flashing
  --build-only     Build the firmware, skip flashing

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
        --skip-bruce)
            # Bruce multi-boot support was removed; accept and ignore the flag
            # so existing invocations / aliases keep working.
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
echo "=== Building this firmware ==="

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    release_serial_port "${PORT}"
fi

# --- Python venv shim -------------------------------------------------------
# ESP-IDF's export.sh derives its virtualenv path from `python3 --version`
# (e.g. ~/.espressif/python_env/idf5.4_py3.13_env). On this machine Homebrew's
# python3 is newer (3.15), so IDF looks for a py3.15 venv that was never created
# and aborts with "virtual environment ... not found". Pin `python3` to the
# interpreter the installed IDF venv was actually built with, via a tiny shim
# dir prepended to PATH. Auto-detects the needed minor version from the venv
# name, so it keeps working after an IDF/python upgrade.
setup_python_shim() {
    local env_root="${IDF_PYTHON_ENV_PATH:-${HOME}/.espressif/python_env}"
    local env_dir pyver py shim
    env_dir="$(ls -d "${env_root}"/idf*_py3.*_env 2>/dev/null | head -1)"
    [[ -z "${env_dir}" ]] && return 0  # no venv to pin to; let IDF handle it
    pyver="$(basename "${env_dir}" | sed -nE 's/.*_py([0-9]+\.[0-9]+)_env/\1/p')"
    [[ -z "${pyver}" ]] && return 0
    # Already the right python3? Nothing to do.
    python3 --version 2>&1 | grep -q " ${pyver}\." && return 0
    py="$(command -v "python${pyver}" 2>/dev/null || true)"
    if [[ -z "${py}" ]]; then
        echo "WARN: IDF venv needs python${pyver}, but 'python${pyver}' is not on PATH." >&2
        echo "      Install it (e.g. 'brew install python@${pyver}') or the build may fail." >&2
        return 0
    fi
    shim="${ESP32_DIR}/.pyshim"
    mkdir -p "${shim}"
    ln -sf "${py}" "${shim}/python3"
    ln -sf "${py}" "${shim}/python"
    export PATH="${shim}:${PATH}"
    echo "Python shim:    python3 -> ${py} (matches IDF venv py${pyver})"
}
setup_python_shim

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

cd "${ESP32_DIR}"

# Board sdkconfig lives in the build dir and is generated from the board
# defaults (USB-JTAG console, dual-core + forced-reboot combo), so other
# boards' builds never share or inherit it.
SDK_OPTS=("-DSDKCONFIG=${BUILD_DIR}/sdkconfig" "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.${BOARD}")

# Set target if build dir doesn't exist yet or target changed
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "Setting IDF target to ${IDF_TARGET}..."
    idf.py -B "${BUILD_DIR}" "${SDK_OPTS[@]}" set-target "${IDF_TARGET}"
fi

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" "${SDK_OPTS[@]}" reconfigure build
    echo
    echo "Build complete (--build-only). Nothing flashed."
    exit 0
fi

idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" "${SDK_OPTS[@]}" -p "${PORT}" reconfigure build flash

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    release_serial_port "${PORT}"
    idf.py -B "${BUILD_DIR}" "${SDK_OPTS[@]}" -p "${PORT}" monitor
fi

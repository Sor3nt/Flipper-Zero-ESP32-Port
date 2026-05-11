#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ESP32_DIR="${SCRIPT_DIR}"
PORT="${ESPPORT:-}"
RUN_MONITOR=0
BUILD_ONLY=0
SKIP_BRUCE=0
EXPORT_SCRIPT="${ESP_IDF_EXPORT_SCRIPT:-${HOME}/esp/esp-idf/export.sh}"

BOARD="lilygo_t_embed_cc1101"
BUILD_DIR="build_t_embed"
IDF_TARGET="esp32s3"

# --- Multi-boot (Bruce in the ota_1 slot) -----------------------------------
# This board ships with two firmwares flashed side by side: this Flipper Zero
# port in ota_0 and the Bruce firmware in ota_1 (see 00_Skills/multi-boot.md
# and partitions_multiboot.csv). buildAndFlash builds both and flashes both.
BRUCE_DIR="${ESP32_DIR}/multi-boot/bruce"
BRUCE_PIO_ENV="lilygo-t-embed-cc1101"
BRUCE_BIN="${BRUCE_DIR}/.pio/build/${BRUCE_PIO_ENV}/firmware.bin"
PATCH_BRUCE="${ESP32_DIR}/patchBruce.py"
PARTITIONS_CSV="${ESP32_DIR}/partitions_multiboot.csv"

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

# Locate a usable PlatformIO entry point (needed to build Bruce).
find_pio() {
    local cand
    for cand in pio platformio "${HOME}/.platformio/penv/bin/pio"; do
        if command -v "${cand}" >/dev/null 2>&1; then
            printf '%s\n' "${cand}"
            return 0
        fi
    done
    if python3 -m platformio --version >/dev/null 2>&1; then
        printf '%s\n' "python3 -m platformio"
        return 0
    fi
    return 1
}

# Read a partition offset (e.g. "ota_1") from partitions_multiboot.csv.
partition_offset() {
    awk -F',' -v name="$1" '
        $1 ~ "^[[:space:]]*"name"[[:space:]]*$" {
            gsub(/[[:space:]]/, "", $4);
            print $4;
            exit
        }' "${PARTITIONS_CSV}"
}

usage() {
    cat <<EOF
Usage: $(basename "$0") [--port <device>] [--monitor] [--build-only] [--skip-bruce]

Builds and flashes the multi-boot image for the LilyGo T-Embed CC1101:
  ota_0 = this ESP32 Flipper Zero port      ota_1 = Bruce firmware

It runs patchBruce.py (clone/pull + patch the bundled Bruce checkout), builds
Bruce with PlatformIO, builds this firmware with ESP-IDF, then flashes both.

Options:
  --port <device>  Serial device to flash. Default: auto-detect /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux)
  --monitor        Open idf.py monitor after flashing
  --build-only     Build both firmwares, skip flashing
  --skip-bruce     Don't touch / build / flash Bruce — only this firmware

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
            SKIP_BRUCE=1
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
if [[ "${SKIP_BRUCE}" -eq 1 ]]; then
    echo "Bruce:          skipped (--skip-bruce)"
else
    echo "Bruce:          ${BRUCE_DIR} (env ${BRUCE_PIO_ENV})"
fi

cd "${ESP32_DIR}"

# ---------------------------------------------------------------------------
# 1) Bruce: update the bundled checkout, patch it, build it with PlatformIO.
#    Done before sourcing ESP-IDF's export.sh so the two toolchains don't mix.
# ---------------------------------------------------------------------------
if [[ "${SKIP_BRUCE}" -eq 0 ]]; then
    echo
    echo "=== Updating + patching Bruce ==="
    python3 "${PATCH_BRUCE}"

    PIO_BIN="$(find_pio || true)"
    if [[ -z "${PIO_BIN}" ]]; then
        echo "PlatformIO not found. Install it, e.g.:" >&2
        echo "    pipx install platformio        # or: pip3 install --user platformio" >&2
        echo "(or rerun with --skip-bruce to build only this firmware)" >&2
        exit 1
    fi

    echo
    echo "=== Building Bruce (${BRUCE_PIO_ENV}) ==="
    # shellcheck disable=SC2086
    ${PIO_BIN} run -d "${BRUCE_DIR}" -e "${BRUCE_PIO_ENV}"

    if [[ ! -f "${BRUCE_BIN}" ]]; then
        echo "Bruce build did not produce ${BRUCE_BIN}" >&2
        exit 1
    fi
    echo "Bruce firmware: ${BRUCE_BIN}"
fi

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

# ---------------------------------------------------------------------------
# 2) This firmware: build (and flash) with ESP-IDF.
# ---------------------------------------------------------------------------
echo
echo "=== Building this firmware ==="

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    release_serial_port "${PORT}"
fi

# shellcheck source=/dev/null
source "${EXPORT_SCRIPT}"

cd "${ESP32_DIR}"

# Set target if build dir doesn't exist yet or target changed
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    echo "Setting IDF target to ${IDF_TARGET}..."
    idf.py -B "${BUILD_DIR}" set-target "${IDF_TARGET}"
fi

# Flash Bruce's app image into the ota_1 slot, and make sure otadata is erased
# so the bootloader boots ota_0 (this firmware) by default.
flash_bruce() {
    [[ "${SKIP_BRUCE}" -eq 1 ]] && return 0
    local ota1_offset otadata_offset
    ota1_offset="$(partition_offset ota_1)"
    otadata_offset="$(partition_offset otadata)"
    if [[ -z "${ota1_offset}" ]]; then
        echo "Could not determine ota_1 offset from ${PARTITIONS_CSV}" >&2
        exit 1
    fi
    echo
    echo "=== Flashing Bruce to ota_1 (${ota1_offset}) ==="
    release_serial_port "${PORT}"
    # otadata is 0x2000 bytes; erasing it (-> 0xFF) makes the bootloader pick ota_0.
    if [[ -n "${otadata_offset}" ]]; then
        esptool.py --chip "${IDF_TARGET}" -p "${PORT}" --before default_reset --after no_reset \
            erase_region "${otadata_offset}" 0x2000
    fi
    esptool.py --chip "${IDF_TARGET}" -p "${PORT}" --before default_reset --after hard_reset \
        write_flash --flash_size detect "${ota1_offset}" "${BRUCE_BIN}"
}

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" reconfigure build
    echo
    echo "Build complete (--build-only). Nothing flashed."
    exit 0
fi

idf.py -B "${BUILD_DIR}" -DFLIPPER_BOARD="${BOARD}" -p "${PORT}" reconfigure build flash
flash_bruce

if [[ "${RUN_MONITOR}" -eq 1 ]]; then
    release_serial_port "${PORT}"
    idf.py -B "${BUILD_DIR}" -p "${PORT}" monitor
fi

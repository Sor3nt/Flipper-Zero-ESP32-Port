#!/bin/bash
#
# Build a Flipper Application Package (.fap) for ESP32 targets.
# Builds for ALL supported targets automatically.
#
# Usage: ./tools/fap_build.sh <app_directory>
#
# Output: build_<board>/fap/<app_name>.fap for each target
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_DIR="$1"

if [ -z "$APP_DIR" ] || [ ! -d "$APP_DIR" ]; then
    echo "Usage: $0 <app_directory>"
    exit 1
fi

# Source ESP-IDF for toolchain access
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
fi

# ── Parse application.fam ───────────────────────────────────────────
ENTRY_POINT="main"
APP_NAME="App"
APP_ICON=""
APP_STACK=16384

if [ -f "$APP_DIR/application.fam" ]; then
    EP=$(grep 'entry_point=' "$APP_DIR/application.fam" | sed 's/.*entry_point="\([^"]*\)".*/\1/' | head -1)
    [ -n "$EP" ] && ENTRY_POINT="$EP"
    NM=$(grep '^\s*name=' "$APP_DIR/application.fam" | sed 's/.*name="\([^"]*\)".*/\1/' | head -1)
    [ -n "$NM" ] && APP_NAME="$NM"
    IC=$(grep 'fap_icon=' "$APP_DIR/application.fam" | sed 's/.*fap_icon="\([^"]*\)".*/\1/' | head -1)
    [ -n "$IC" ] && APP_ICON="$APP_DIR/$IC"
    SK=$(grep 'stack_size=' "$APP_DIR/application.fam" | sed 's/.*stack_size=\([0-9*  ]*\).*/\1/' | head -1)
    if [ -n "$SK" ]; then
        APP_STACK=$(python3 -c "print(max(int($SK), 16384))" 2>/dev/null || echo 16384)
    fi
fi

APP_ID=$(grep 'appid=' "$APP_DIR/application.fam" 2>/dev/null | sed 's/.*appid="\([^"]*\)".*/\1/' | head -1)
[ -z "$APP_ID" ] && APP_ID=$(basename "$APP_DIR")
FAP_FILENAME="${APP_ID}.fap"

# ── Target definitions ──────────────────────────────────────────────
#           BOARD_NAME              IDF_TARGET  TOOLCHAIN_PREFIX        BUILD_DIR
TARGETS=(
    "lilygo_t_embed_cc1101      esp32s3     xtensa-esp32s3-elf      build_t_embed"
    "waveshare_c6_1.9           esp32c6     riscv32-esp-elf         build_waveshare_c6"
)

# ── Common include paths (project-level) ────────────────────────────
COMMON_INCLUDES=(
    -I"$PROJECT_DIR/components"
    -I"$PROJECT_DIR"
    -I"$PROJECT_DIR/components/furi"
    -I"$PROJECT_DIR/components/furi/core"
    -I"$PROJECT_DIR/components/mlib"
    -I"$PROJECT_DIR/components/toolbox"
    -I"$PROJECT_DIR/components/toolbox/stream"
    -I"$PROJECT_DIR/lib/toolbox/protocols"
    -I"$PROJECT_DIR/lib/toolbox/pulse_protocols"
    -I"$PROJECT_DIR/components/storage"
    -I"$PROJECT_DIR/components/input"
    -I"$PROJECT_DIR/components/gui"
    -I"$PROJECT_DIR/components/gui/modules"
    -I"$PROJECT_DIR/components/gui/modules/widget_elements"
    -I"$PROJECT_DIR/components/notification"
    -I"$PROJECT_DIR/components/assets"
    -I"$PROJECT_DIR/components/loader"
    -I"$PROJECT_DIR/components/flipper_application"
    -I"$PROJECT_DIR/components/flipper_format"
    -I"$PROJECT_DIR/components/dialogs"
    -I"$PROJECT_DIR/components/locale"
    -I"$PROJECT_DIR/components/u8g2"
    -I"$PROJECT_DIR/components/furi_hal"
    -I"$PROJECT_DIR/components/furi_hal/boards"
    -I"$PROJECT_DIR/components/subghz"
    -I"$PROJECT_DIR/components/bit_lib"
    -I"$PROJECT_DIR/components/archive"
    -I"$PROJECT_DIR/components/nfc"
    -I"$PROJECT_DIR/components/infrared"
    -I"$PROJECT_DIR/components/lfrfid"
    -I"$PROJECT_DIR/targets"
    -I"$PROJECT_DIR/lib/subghz"
)

# ESP-IDF common includes
IDF="$HOME/esp/esp-idf/components"
IDF_COMMON_INCLUDES=(
    -I"$IDF/newlib/platform_include"
    -I"$IDF/esp_hw_support/include"
    -I"$IDF/esp_hw_support/include/soc"
    -I"$IDF/heap/include"
    -I"$IDF/log/include"
    -I"$IDF/esp_rom/include"
    -I"$IDF/esp_common/include"
    -I"$IDF/esp_system/include"
    -I"$IDF/esp_timer/include"
    -I"$IDF/esp_event/include"
    -I"$IDF/esp_driver_gpio/include"
    -I"$IDF/esp_driver_rmt/include"
    -I"$IDF/esp_driver_spi/include"
    -I"$IDF/esp_driver_i2c/include"
    -I"$IDF/esp_driver_uart/include"
    -I"$IDF/esp_ringbuf/include"
    -I"$IDF/esp_partition/include"
    -I"$IDF/fatfs/diskio"
    -I"$IDF/fatfs/src"
    -I"$IDF/fatfs/vfs"
    -I"$IDF/vfs/include"
    -I"$IDF/wear_levelling/include"
    -I"$IDF/sdmmc/include"
    -I"$IDF/nvs_flash/include"
    -I"$IDF/bt/include/esp32c3/include"
    -I"$IDF/bt/host/bluedroid/api/include/api"
    -I"$IDF/lwip/include"
    -I"$IDF/lwip/port/include"
    -I"$IDF/lwip/port/freertos/include"
    -I"$IDF/lwip/port/esp32xx/include"
    -I"$IDF/driver/deprecated"
    -I"$IDF/driver/i2c/include"
    -I"$IDF/esp_adc/include"
)

# ── Common compiler flags ───────────────────────────────────────────
COMMON_CFLAGS=(
    -fno-common
    -ffunction-sections
    -fdata-sections
    -fno-builtin
    -fno-jump-tables
    -fno-tree-switch-conversion
    -std=gnu17
    -Wall
    -Wno-unused-parameter
    -Wno-sign-compare
    -Os
    -DESP_PLATFORM
    -DIDF_VER=\"v5.4.1\"
    -DSOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE
    -DSOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ
)

# ── Build function for one target ───────────────────────────────────
build_for_target() {
    local BOARD="$1"
    local IDF_TARGET="$2"
    local TOOLCHAIN="$3"
    local FW_BUILD_DIR="$4"

    local CC="${TOOLCHAIN}-gcc"
    local LD="${TOOLCHAIN}-ld"
    local OBJCOPY="${TOOLCHAIN}-objcopy"
    local READELF="${TOOLCHAIN}-readelf"

    if ! command -v "$CC" &>/dev/null; then
        echo "  SKIP $BOARD: $CC not found"
        return 0
    fi

    if [ ! -d "$PROJECT_DIR/$FW_BUILD_DIR/config" ]; then
        echo "  SKIP $BOARD: firmware not built ($FW_BUILD_DIR/config missing)"
        return 0
    fi

    local BUILD_DIR="$PROJECT_DIR/$FW_BUILD_DIR/fap/$(basename "$APP_DIR")"
    local OUTPUT_DIR="$PROJECT_DIR/$FW_BUILD_DIR/fap"
    local OUTPUT="$OUTPUT_DIR/$FAP_FILENAME"
    mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

    echo ""
    echo "━━━ Building for $BOARD ($IDF_TARGET) ━━━"

    # Target-specific includes & flags
    local -a TARGET_INCLUDES=()
    local -a TARGET_CFLAGS=()

    TARGET_INCLUDES+=(-I"$PROJECT_DIR/$FW_BUILD_DIR/config")
    TARGET_INCLUDES+=(-I"$IDF/esp_hw_support/include/soc/$IDF_TARGET")

    if [ "$IDF_TARGET" = "esp32s3" ]; then
        TARGET_CFLAGS+=(-mlongcalls)
        TARGET_INCLUDES+=(
            -I"$IDF/freertos/config/xtensa/include"
            -I"$IDF/freertos/config/include"
            -I"$IDF/freertos/config/include/freertos"
            -I"$IDF/freertos/FreeRTOS-Kernel/include"
            -I"$IDF/freertos/FreeRTOS-Kernel/portable/xtensa/include"
            -I"$IDF/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos"
            -I"$IDF/freertos/esp_additions/include"
            -I"$IDF/xtensa/esp32s3/include"
            -I"$IDF/xtensa/include"
            -I"$IDF/xtensa/deprecated_include"
            -I"$IDF/soc/esp32s3/include"
            -I"$IDF/soc/esp32s3/register"
            -I"$IDF/soc/esp32s3"
            -I"$IDF/soc/include"
            -I"$IDF/hal/esp32s3/include"
            -I"$IDF/hal/include"
            -I"$IDF/hal/platform_port/include"
            -I"$IDF/esp_hw_support/port/esp32s3/."
            -I"$IDF/esp_hw_support/port/esp32s3/include"
            -I"$IDF/esp_rom/esp32s3/include"
            -I"$IDF/esp_rom/esp32s3/include/esp32s3"
            -I"$IDF/esp_rom/esp32s3"
        )
        TARGET_CFLAGS+=(-DBOARD_INCLUDE=\"board_lilygo_t_embed_cc1101.h\")
    elif [ "$IDF_TARGET" = "esp32c6" ]; then
        TARGET_CFLAGS+=(-march=rv32imac_zicsr_zifencei)
        TARGET_INCLUDES+=(
            -I"$IDF/freertos/config/riscv/include"
            -I"$IDF/freertos/config/include"
            -I"$IDF/freertos/config/include/freertos"
            -I"$IDF/freertos/FreeRTOS-Kernel/include"
            -I"$IDF/freertos/FreeRTOS-Kernel/portable/riscv/include"
            -I"$IDF/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos"
            -I"$IDF/freertos/esp_additions/include"
            -I"$IDF/riscv/include"
            -I"$IDF/soc/esp32c6/include"
            -I"$IDF/soc/esp32c6/register"
            -I"$IDF/soc/esp32c6"
            -I"$IDF/soc/include"
            -I"$IDF/hal/esp32c6/include"
            -I"$IDF/hal/include"
            -I"$IDF/hal/platform_port/include"
            -I"$IDF/esp_hw_support/port/esp32c6/."
            -I"$IDF/esp_hw_support/port/esp32c6/include"
            -I"$IDF/esp_rom/esp32c6/include"
            -I"$IDF/esp_rom/esp32c6/include/esp32c6"
            -I"$IDF/esp_rom/esp32c6"
        )
        TARGET_CFLAGS+=(-DBOARD_INCLUDE=\"board_waveshare_c6_1.9.h\")
    fi

    TARGET_CFLAGS+=(-DFAP_VERSION=\"1.0\")

    # Generate icon assets from fap_icon_assets if defined in application.fam
    local ICON_ASSETS_DIR=""
    if [ -f "$APP_DIR/application.fam" ]; then
        local IAD=$(grep 'fap_icon_assets=' "$APP_DIR/application.fam" | sed 's/.*fap_icon_assets="\([^"]*\)".*/\1/' | head -1)
        [ -n "$IAD" ] && ICON_ASSETS_DIR="$APP_DIR/$IAD"
    fi

    local ICONS_GEN_DIR="$BUILD_DIR/icons"
    if [ -n "$ICON_ASSETS_DIR" ] && [ -d "$ICON_ASSETS_DIR" ]; then
        # Detect icon filename from source includes (e.g. #include "proto_pirate_icons.h")
        local ICON_STEM="${APP_ID}_icons"
        local DETECTED=$(grep -rh '#include ".*_icons\.h"' "$APP_DIR" 2>/dev/null | head -1 | sed 's/.*"\(.*\)\.h".*/\1/')
        [ -n "$DETECTED" ] && ICON_STEM="$DETECTED"

        mkdir -p "$ICONS_GEN_DIR"
        python3 "$SCRIPT_DIR/fam/compile_icons.py" icons \
            --filename "$ICON_STEM" \
            "$ICON_ASSETS_DIR" "$ICONS_GEN_DIR" 2>/dev/null || true
        if [ -f "$ICONS_GEN_DIR/${ICON_STEM}.h" ]; then
            TARGET_INCLUDES+=(-I"$ICONS_GEN_DIR")
        fi
    fi

    # Find source files
    local -a SOURCES=($(find "$APP_DIR" -name '*.c' -type f))
    # Add generated icon .c files
    if [ -d "$ICONS_GEN_DIR" ]; then
        for icon_src in "$ICONS_GEN_DIR"/*.c; do
            [ -f "$icon_src" ] && SOURCES+=("$icon_src")
        done
    fi

    echo "  Sources: ${#SOURCES[@]} files"

    # Compile
    local -a OBJECTS=()
    for src in "${SOURCES[@]}"; do
        local obj="$BUILD_DIR/$(echo "$src" | sed 's|/|_|g' | sed 's|\.c$|.o|')"
        OBJECTS+=("$obj")

        "$CC" "${COMMON_CFLAGS[@]}" "${TARGET_CFLAGS[@]}" \
            "${COMMON_INCLUDES[@]}" "${IDF_COMMON_INCLUDES[@]}" "${TARGET_INCLUDES[@]}" \
            -I"$APP_DIR" -I"$APP_DIR/helpers" -I"$APP_DIR/scenes" \
            -I"$APP_DIR/views" -I"$APP_DIR/protocols" \
            -c "$src" -o "$obj"
    done

    # Link
    echo "  Linking ${#OBJECTS[@]} objects (entry=$ENTRY_POINT)"
    "$LD" -r -T "$SCRIPT_DIR/fap.ld" --entry="$ENTRY_POINT" -o "$BUILD_DIR/app.elf" "${OBJECTS[@]}"
    local SECTIONS=$("$READELF" -S "$BUILD_DIR/app.elf" | grep -c '^\s*\[')

    # Manifest with icon
    local ICON_ARG=""
    if [ -n "$APP_ICON" ] && [ -f "$APP_ICON" ]; then
        ICON_ARG="--icon $APP_ICON"
    fi
    python3 "$SCRIPT_DIR/fap_manifest.py" \
        --name "$APP_NAME" \
        --api-major 1 --api-minor 0 \
        --target 32 \
        --stack-size "$APP_STACK" \
        --app-version 1 \
        $ICON_ARG \
        --output "$BUILD_DIR/manifest.bin"

    # Inject manifest into ELF
    "$OBJCOPY" --add-section .fapmeta="$BUILD_DIR/manifest.bin" \
        --set-section-flags .fapmeta=contents,readonly \
        "$BUILD_DIR/app.elf" "$OUTPUT"

    local SIZE=$(wc -c < "$OUTPUT")
    echo "  ✓ $OUTPUT ($SIZE bytes, $SECTIONS sections)"
}

# ── Main: build for all targets ─────────────────────────────────────
echo "╔══════════════════════════════════════════════════╗"
echo "║  FAP Build: $APP_NAME ($FAP_FILENAME)           "
echo "╚══════════════════════════════════════════════════╝"

for target_line in "${TARGETS[@]}"; do
    read -r BOARD IDF_TARGET TOOLCHAIN FW_BUILD_DIR <<< "$target_line"
    build_for_target "$BOARD" "$IDF_TARGET" "$TOOLCHAIN" "$FW_BUILD_DIR"
done

echo ""
echo "Done."

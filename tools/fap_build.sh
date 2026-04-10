#!/bin/bash
#
# Build a Flipper Application Package (.fap) for ESP32-S3/Xtensa
#
# Usage: ./tools/fap_build.sh <app_directory> [output.fap]
#
# Example:
#   ./tools/fap_build.sh applications_user/protopirate_fap_test proto_pirate.fap
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_DIR="$1"
OUTPUT="${2:-$(basename "$APP_DIR").fap}"

if [ -z "$APP_DIR" ] || [ ! -d "$APP_DIR" ]; then
    echo "Usage: $0 <app_directory> [output.fap]"
    echo "  app_directory: path to the FAP application source"
    exit 1
fi

# Find the Xtensa toolchain
TOOLCHAIN_PREFIX="xtensa-esp32s3-elf"
CC="${TOOLCHAIN_PREFIX}-gcc"
LD="${TOOLCHAIN_PREFIX}-ld"
OBJCOPY="${TOOLCHAIN_PREFIX}-objcopy"

if ! command -v "$CC" &>/dev/null; then
    # Try to source ESP-IDF
    if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
    fi
    if ! command -v "$CC" &>/dev/null; then
        echo "Error: $CC not found. Source ESP-IDF export.sh first."
        exit 1
    fi
fi

# Include paths - mirror what ESP-IDF uses for the firmware build
INCLUDE_DIRS=(
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
    # ESP-IDF includes from build config
    -I"$PROJECT_DIR/build/config"
    # ESP-IDF system headers
    -I"$HOME/esp/esp-idf/components/newlib/platform_include"
    -I"$HOME/esp/esp-idf/components/freertos/config/include"
    -I"$HOME/esp/esp-idf/components/freertos/config/include/freertos"
    -I"$HOME/esp/esp-idf/components/freertos/config/xtensa/include"
    -I"$HOME/esp/esp-idf/components/freertos/FreeRTOS-Kernel/include"
    -I"$HOME/esp/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include"
    -I"$HOME/esp/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos"
    -I"$HOME/esp/esp-idf/components/freertos/esp_additions/include"
    -I"$HOME/esp/esp-idf/components/esp_hw_support/include"
    -I"$HOME/esp/esp-idf/components/esp_hw_support/include/soc"
    -I"$HOME/esp/esp-idf/components/esp_hw_support/include/soc/esp32s3"
    -I"$HOME/esp/esp-idf/components/heap/include"
    -I"$HOME/esp/esp-idf/components/log/include"
    -I"$HOME/esp/esp-idf/components/soc/include"
    -I"$HOME/esp/esp-idf/components/soc/esp32s3"
    -I"$HOME/esp/esp-idf/components/soc/esp32s3/include"
    -I"$HOME/esp/esp-idf/components/soc/esp32s3/register"
    -I"$HOME/esp/esp-idf/components/hal/platform_port/include"
    -I"$HOME/esp/esp-idf/components/hal/esp32s3/include"
    -I"$HOME/esp/esp-idf/components/hal/include"
    -I"$HOME/esp/esp-idf/components/esp_rom/include"
    -I"$HOME/esp/esp-idf/components/esp_rom/esp32s3/include"
    -I"$HOME/esp/esp-idf/components/esp_rom/esp32s3/include/esp32s3"
    -I"$HOME/esp/esp-idf/components/esp_common/include"
    -I"$HOME/esp/esp-idf/components/esp_system/include"
    -I"$HOME/esp/esp-idf/components/xtensa/esp32s3/include"
    -I"$HOME/esp/esp-idf/components/xtensa/include"
    -I"$HOME/esp/esp-idf/components/xtensa/deprecated_include"
    -I"$HOME/esp/esp-idf/components/esp_timer/include"
    -I"$HOME/esp/esp-idf/components/esp_driver_gpio/include"
    -I"$HOME/esp/esp-idf/components/esp_driver_rmt/include"
    -I"$HOME/esp/esp-idf/components/esp_driver_spi/include"
    -I"$HOME/esp/esp-idf/components/esp_driver_i2c/include"
    -I"$HOME/esp/esp-idf/components/esp_driver_uart/include"
    -I"$HOME/esp/esp-idf/components/fatfs/diskio"
    -I"$HOME/esp/esp-idf/components/fatfs/src"
    -I"$HOME/esp/esp-idf/components/fatfs/vfs"
    -I"$HOME/esp/esp-idf/components/vfs/include"
    -I"$HOME/esp/esp-idf/components/esp_event/include"
    -I"$HOME/esp/esp-idf/components/esp_partition/include"
    -I"$HOME/esp/esp-idf/components/wear_levelling/include"
    -I"$HOME/esp/esp-idf/components/esp_ringbuf/include"
    -I"$HOME/esp/esp-idf/components/driver/deprecated"
    -I"$HOME/esp/esp-idf/components/driver/i2c/include"
    -I"$HOME/esp/esp-idf/components/sdmmc/include"
    -I"$HOME/esp/esp-idf/components/esp_adc/include"
    -I"$HOME/esp/esp-idf/components/lwip/include"
    -I"$HOME/esp/esp-idf/components/lwip/port/include"
    -I"$HOME/esp/esp-idf/components/lwip/port/freertos/include"
    -I"$HOME/esp/esp-idf/components/lwip/port/esp32xx/include"
    -I"$HOME/esp/esp-idf/components/bt/include/esp32c3/include"
    -I"$HOME/esp/esp-idf/components/bt/host/bluedroid/api/include/api"
    -I"$HOME/esp/esp-idf/components/nvs_flash/include"
)

# Compiler flags
CFLAGS=(
    -mlongcalls
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
    -DBOARD_INCLUDE=\"board_waveshare_c6_1.9.h\"
    -DFAP_VERSION=\"2.6\"
)

# Build directory
BUILD_DIR="$PROJECT_DIR/build/fap_build/$(basename "$APP_DIR")"
mkdir -p "$BUILD_DIR"

# Check for pre-generated icon files (from a previous firmware build)
ICON_DIR=""
for candidate in \
    "$PROJECT_DIR/build/fap_build/$(basename "$APP_DIR")/icons" \
    "$PROJECT_DIR/build_waveshare_c6/esp-idf/main/generated/icons/proto_pirate" \
    "$PROJECT_DIR/build_t_embed/esp-idf/main/generated/icons/proto_pirate" \
    "$PROJECT_DIR/build/esp-idf/main/generated/icons/proto_pirate"; do
    if [ -d "$candidate" ]; then
        ICON_DIR="$candidate"
        INCLUDE_DIRS+=(-I"$ICON_DIR")
        echo "Using icons from: $ICON_DIR"
        break
    fi
done

# Find all .c source files in the app directory
echo "=== Building FAP from $APP_DIR ==="
SOURCES=($(find "$APP_DIR" -name '*.c' -type f))
# Add icon .c files if found
if [ -n "$ICON_DIR" ]; then
    for icon_src in "$ICON_DIR"/*.c; do
        [ -f "$icon_src" ] && SOURCES+=("$icon_src")
    done
fi
echo "Found ${#SOURCES[@]} source files"

# Compile each source file
OBJECTS=()
for src in "${SOURCES[@]}"; do
    obj="$BUILD_DIR/$(echo "$src" | sed 's|/|_|g' | sed 's|\.c$|.o|')"
    OBJECTS+=("$obj")

    # Add app directory as include path
    echo "  CC $(basename "$src")"
    "$CC" "${CFLAGS[@]}" "${INCLUDE_DIRS[@]}" \
        -I"$APP_DIR" \
        -I"$APP_DIR/helpers" \
        -I"$APP_DIR/scenes" \
        -I"$APP_DIR/views" \
        -I"$APP_DIR/protocols" \
        -c "$src" -o "$obj"
done

# Detect entry point from application.fam
ENTRY_POINT="main"
if [ -f "$APP_DIR/application.fam" ]; then
    EP=$(grep 'entry_point=' "$APP_DIR/application.fam" | sed 's/.*entry_point="\([^"]*\)".*/\1/' | head -1)
    [ -n "$EP" ] && ENTRY_POINT="$EP"
fi
echo "=== Linking ${#OBJECTS[@]} objects (entry=$ENTRY_POINT) ==="
# Link as relocatable ELF (partial link) with section merging
"$LD" -r -T "$SCRIPT_DIR/fap.ld" --entry="$ENTRY_POINT" -o "$BUILD_DIR/app.elf" "${OBJECTS[@]}"

# Show section count
echo "Sections: $(${TOOLCHAIN_PREFIX}-readelf -S "$BUILD_DIR/app.elf" | grep -c '^\s*\[')"

echo "=== Generating manifest ==="
# Generate the .fapmeta section
python3 "$SCRIPT_DIR/fap_manifest.py" \
    --name "ProtoPirate" \
    --api-major 1 \
    --api-minor 0 \
    --target 32 \
    --stack-size 16384 \
    --app-version 1 \
    --output "$BUILD_DIR/manifest.bin"

# Inject .fapmeta section into ELF
"$OBJCOPY" --add-section .fapmeta="$BUILD_DIR/manifest.bin" \
    --set-section-flags .fapmeta=contents,readonly \
    "$BUILD_DIR/app.elf" "$OUTPUT"

echo "=== FAP built: $OUTPUT ==="
echo "Size: $(wc -c < "$OUTPUT") bytes"
echo ""
echo "Relocations:"
"${TOOLCHAIN_PREFIX}-readelf" -r "$OUTPUT" | head -20

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
PROJECT_DIR="$(cd "$SCRIPT_DIR" && pwd)"

APP_DIR="$1"

if [ -z "$APP_DIR" ] || [ ! -d "$APP_DIR" ]; then
    echo "Usage: $0 <app_directory>"
    exit 1
fi

# Source ESP-IDF for toolchain access. Honor ESP_IDF_DIR if set; fall back to
# the canonical ~/esp/esp-idf path; finally try the Windows default install.
if [ -z "$IDF_PATH" ]; then
    if [ -n "$ESP_IDF_DIR" ] && [ -f "$ESP_IDF_DIR/export.sh" ]; then
        . "$ESP_IDF_DIR/export.sh" >/dev/null 2>&1
    elif [ -f "$HOME/esp/esp-idf/export.sh" ]; then
        . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
    elif [ -f "/c/Espressif/frameworks/esp-idf-v5.4.1/export.sh" ]; then
        . "/c/Espressif/frameworks/esp-idf-v5.4.1/export.sh" >/dev/null 2>&1
    fi
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

# ── Plugin (.fal) build overrides (optional env vars) ───────────────
# Build a single source file as a relocatable plugin instead of the whole
# app dir. Used to (re)build the supported_cards NDEF parsers for ESP32:
#   FAP_SINGLE_SOURCE   - compile only this .c (not `find $APP_DIR`)
#   FAP_ENTRY_OVERRIDE  - ELF entry symbol (e.g. ndef_plugin_ep)
#   FAP_CDEFINES        - extra -D flags (e.g. -DNDEF_PROTO=1)
#   FAP_STACK_OVERRIDE  - manifest stack_size; 0 marks it as a plugin
#                         (flipper_application_is_plugin: stack_size == 0)
#   FAP_OUTPUT_NAME     - output filename (e.g. ndef_ul_parser.fal)
[ -n "$FAP_ENTRY_OVERRIDE" ] && ENTRY_POINT="$FAP_ENTRY_OVERRIDE"
[ -n "$FAP_OUTPUT_NAME" ] && FAP_FILENAME="$FAP_OUTPUT_NAME"
# -n on the literal string: "0" is non-empty, so a 0 override is honored.
[ -n "$FAP_STACK_OVERRIDE" ] && APP_STACK="$FAP_STACK_OVERRIDE"

# ── Target definitions ──────────────────────────────────────────────
#           BOARD_NAME              IDF_TARGET  TOOLCHAIN_PREFIX        BUILD_DIR
TARGETS=(
    "lilygo_t_embed_cc1101      esp32s3     xtensa-esp32s3-elf      build_t_embed"
#    "waveshare_c6_1.9           esp32c6     riscv32-esp-elf         build_waveshare_c6"
)

# ── Common include paths (project-level) ────────────────────────────
COMMON_INCLUDES=(
    -I"$PROJECT_DIR/components/compat"
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
    -I"$PROJECT_DIR/applications/services"
    -I"$PROJECT_DIR/components/flipper_application"
    -I"$PROJECT_DIR/components/flipper_format"
    -I"$PROJECT_DIR/components/dialogs"
    -I"$PROJECT_DIR/components/locale"
    -I"$PROJECT_DIR/components/u8g2"
    -I"$PROJECT_DIR/components/furi_hal"
    -I"$PROJECT_DIR/components/furi_hal/boards"
    -I"$PROJECT_DIR/components/furi_ble"
    -I"$PROJECT_DIR/components/ble_profile"
    -I"$PROJECT_DIR/components/bt"
    -I"$PROJECT_DIR/components/btshim"
    -I"$PROJECT_DIR/components/subghz"
    -I"$PROJECT_DIR/components/bit_lib"
    -I"$PROJECT_DIR/components/archive"
    -I"$PROJECT_DIR/components/nfc"
    -I"$PROJECT_DIR/components/infrared"
    -I"$PROJECT_DIR/components/lfrfid"
    -I"$PROJECT_DIR/components/ble_serial"
    -I"$PROJECT_DIR/targets"
    -I"$PROJECT_DIR/lib/subghz"
)

# ESP-IDF common includes — derive from IDF_PATH (set by export.sh) when present.
if [ -n "$IDF_PATH" ] && [ -d "$IDF_PATH/components" ]; then
    IDF="$IDF_PATH/components"
else
    IDF="$HOME/esp/esp-idf/components"
fi
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
    -I"$IDF/lwip/lwip/src/include"
    -I"$IDF/lwip/port/include"
    -I"$IDF/lwip/port/freertos/include"
    -I"$IDF/lwip/port/esp32xx/include"
    -I"$IDF/esp_wifi/include"
    -I"$IDF/esp_netif/include"
    -I"$IDF/driver/deprecated"
    -I"$IDF/driver/i2c/include"
    -I"$IDF/esp_driver_i2s/include"
    -I"$IDF/esp_adc/include"
    -I"$IDF/mbedtls/port/include"
    -I"$IDF/mbedtls/mbedtls/include"
    -I"$IDF/esp_lcd/include"
    -I"$IDF/esp_lcd/interface"
    -I"$IDF/esp_lcd/rgb/include"
    -I"$IDF/esp_lcd/priv_include"
)

# ── Common compiler flags ───────────────────────────────────────────
COMMON_CFLAGS=(
    # Force malloc() -> calloc() (zeroed heap) like the STM32 firmware does in
    # components/furi/core/memmgr.h. The ESP32 heap is NOT zeroed; upstream apps
    # routinely rely on calloc-like behavior (struct pointer fields left unset
    # then guarded by `if(p) free(p)`), which is garbage -> crash on ESP32.
    -include "$SCRIPT_DIR/tools/fap_zeroed_heap.h"
    -D_GNU_SOURCE
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
    # GCC 14 promoted these three C warnings to hard errors by default. Upstream
    # Flipper apps were written for older toolchains where they were warnings, so
    # keep them as warnings — exactly what the firmware components do (see e.g.
    # components/infrared/CMakeLists.txt, lfrfid, furi_hal). Purely additive:
    # downgrading errors to warnings can never break a build that already passed.
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=implicit-function-declaration
    -Os
    -g
    -DESP_PLATFORM
    -DIDF_VER=\"v5.4.1\"
    -DSOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE
    -DSOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ
)

COMMON_CXXFLAGS=(
    -fno-common
    -ffunction-sections
    -fdata-sections
    -fno-builtin
    -fno-jump-tables
    -fno-tree-switch-conversion
    -std=gnu++17
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wno-unused-parameter
    -Wno-sign-compare
    -Os
    -g
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
    local CXX="${TOOLCHAIN}-g++"
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
    # dirname of OUTPUT so FAP_OUTPUT_NAME may carry a subpath (embedded plugins
    # land under <appid>_assets/plugins/<plugin>.fal).
    mkdir -p "$BUILD_DIR" "$OUTPUT_DIR" "$(dirname "$OUTPUT")"

    echo ""
    echo "━━━ Building for $BOARD ($IDF_TARGET) ━━━"

    # Target-specific includes & flags
    local -a TARGET_INCLUDES=()
    local -a TARGET_CFLAGS=()

    TARGET_INCLUDES+=(-I"$PROJECT_DIR/$FW_BUILD_DIR/config")
    TARGET_INCLUDES+=(-I"$IDF/esp_hw_support/include/soc/$IDF_TARGET")

    if [ "$IDF_TARGET" = "esp32s3" ]; then
        # -mlongcalls: allow calls beyond the ±512KB direct-call range (needed as
        #   the FAP + firmware end up far apart in memory).
        # -mtext-section-literals: place each function's L32R literals INSIDE its
        #   .text.<func> section (next to the code) instead of in separate
        #   .literal sections. Xtensa L32R only reaches ~256KB backwards; on large
        #   FAPs (e.g. wolf3d) the split-out literal pool lands out of range and
        #   the ELF loader fails with "L32R offset out of range". Mirrors ESP-IDF.
        TARGET_CFLAGS+=(-mlongcalls -mtext-section-literals)
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
    [ -n "$FAP_CDEFINES" ] && TARGET_CFLAGS+=($FAP_CDEFINES)
    # Extra compiler flags (space-separated), e.g. to relax GCC 14 warnings
    # that upstream apps written for older toolchains trip over:
    #   FAP_EXTRA_CFLAGS="-Wno-incompatible-pointer-types -Wno-int-conversion"
    [ -n "$FAP_EXTRA_CFLAGS" ] && TARGET_CFLAGS+=($FAP_EXTRA_CFLAGS)

    # Generate icon assets from fap_icon_assets if defined in application.fam
    local ICON_ASSETS_DIR=""
    if [ -f "$APP_DIR/application.fam" ]; then
        local IAD=$(grep 'fap_icon_assets=' "$APP_DIR/application.fam" | sed 's/.*fap_icon_assets="\([^"]*\)".*/\1/' | head -1)
        [ -n "$IAD" ] && ICON_ASSETS_DIR="$APP_DIR/$IAD"
    fi

    local ICONS_GEN_DIR="$BUILD_DIR/icons"
    if [ -n "$ICON_ASSETS_DIR" ] && [ -d "$ICON_ASSETS_DIR" ]; then
        # Detect icon header stem(s) from #include "..._icons.h". Scan only the
        # sources actually being built (FAP_SOURCES/FAP_SINGLE_SOURCE for an
        # embedded plugin) so a plugin gets its OWN header name — e.g. psa_bf
        # includes protopirate_psa_bf_plugin_icons.h, not proto_pirate_icons.h.
        local ICON_SCAN="$APP_DIR"
        [ -n "$FAP_SOURCES" ] && ICON_SCAN="$FAP_SOURCES"
        [ -n "$FAP_SINGLE_SOURCE" ] && ICON_SCAN="$FAP_SINGLE_SOURCE"
        local ICON_STEMS
        ICON_STEMS=$(grep -rh '#include ".*_icons\.h"' $ICON_SCAN 2>/dev/null | sed 's/.*"\(.*\)\.h".*/\1/' | sort -u)
        [ -z "$ICON_STEMS" ] && ICON_STEMS="${APP_ID}_icons"

        mkdir -p "$ICONS_GEN_DIR"
        # Generate every referenced header from the same assets dir. All stems get
        # identical icon symbols (named by PNG file), so keep only the FIRST .c to
        # avoid duplicate definitions at link; the extra headers stay declaration-only.
        local _icon_first=1
        for stem in $ICON_STEMS; do
            python3 "$SCRIPT_DIR/tools/fam/compile_icons.py" icons \
                --filename "$stem" \
                "$ICON_ASSETS_DIR" "$ICONS_GEN_DIR" 2>/dev/null || true
            if [ "$_icon_first" = "1" ] && [ -f "$ICONS_GEN_DIR/${stem}.h" ]; then
                _icon_first=0
            else
                [ -f "$ICONS_GEN_DIR/${stem}.c" ] && rm -f "$ICONS_GEN_DIR/${stem}.c"
            fi
        done
        TARGET_INCLUDES+=(-I"$ICONS_GEN_DIR")
    fi

    # Honor fap_private_libs[].sources: apps may vendor a big library (e.g.
    # wolfssl) but only build a subset of it. fap_lib_info.py reads the .fam and
    # tells us which lib dirs to exclude from the blanket glob, which lib sources
    # to build instead, and the per-lib cdefines/cincludes/cflags to apply.
    local -a LIB_EXCLUDE_DIRS=()
    local -a LIB_SOURCES=()
    if [ -f "$APP_DIR/application.fam" ]; then
        local LIB_INFO
        LIB_INFO=$(python3 "$SCRIPT_DIR/tools/fap_lib_info.py" "$APP_DIR" 2>/dev/null || true)
        while IFS=$'\t' read -r kind val; do
            case "$kind" in
                LIBDIR)   LIB_EXCLUDE_DIRS+=("$APP_DIR/lib/$val") ;;
                SOURCE)   LIB_SOURCES+=("$APP_DIR/$val") ;;
                CDEFINE)  TARGET_CFLAGS+=("-D$val") ;;
                CINCLUDE) TARGET_INCLUDES+=(-I"$APP_DIR/$val") ;;
                CFLAG)    TARGET_CFLAGS+=("$val") ;;
            esac
        done <<< "$LIB_INFO"
    fi

    # Honor an explicit main-app `sources=[...]` list in the .fam (fbt glob/exclude
    # semantics). Needed for apps with a plugin architecture (e.g. protopirate)
    # that exclude their plugin *template* .c files from the main app — blanket-
    # globbing those trips their `#error "PP_* must be defined"` guards. Emits
    # nothing for apps that build "everything", so they keep the plain glob below.
    # Skip entirely for explicit-source sub-builds (FAP_SOURCES / FAP_SINGLE_SOURCE,
    # e.g. an embedded plugin): the main app's fam sources/cdefines must NOT leak
    # into them — a leaked PROTOPIRATE_PROTOCOL_RX_ONLY would flip the TX plugins'
    # PROTOPIRATE_WITH_ENCODER to 0 and hide the encoder API they use.
    local -a FAM_SOURCES=()
    if [ -z "$FAP_SOURCES" ] && [ -z "$FAP_SINGLE_SOURCE" ] && [ -f "$APP_DIR/application.fam" ]; then
        local APP_INFO
        APP_INFO=$(python3 "$SCRIPT_DIR/tools/fap_app_info.py" "$APP_DIR" 2>/dev/null || true)
        while IFS=$'\t' read -r kind val; do
            case "$kind" in
                APPSOURCE)  [ -n "$val" ] && FAM_SOURCES+=("$APP_DIR/$val") ;;
                APPCDEFINE) [ -n "$val" ] && TARGET_CFLAGS+=("-D$val") ;;
            esac
        done <<< "$APP_INFO"
    fi

    # Find source files. Priority:
    #   FAP_SOURCES        - explicit space-separated list (multi-source plugin)
    #   FAP_SINGLE_SOURCE  - one source (single-source plugin)
    #   FAM_SOURCES        - main-app sources resolved from the .fam (see above)
    #   else               - whole app dir, minus excluded private-lib dirs,
    #                        plus the selected private-lib sources
    local -a C_SOURCES=()
    local -a CXX_SOURCES=()
    if [ -n "$FAP_SOURCES" ]; then
        C_SOURCES=($FAP_SOURCES)
    elif [ -n "$FAP_SINGLE_SOURCE" ]; then
        C_SOURCES=("$FAP_SINGLE_SOURCE")
    elif [ ${#FAM_SOURCES[@]} -gt 0 ]; then
        # Split the fam-resolved list by extension; add private-lib sources too.
        for s in "${FAM_SOURCES[@]}" "${LIB_SOURCES[@]}"; do
            case "$s" in
                *.cpp) CXX_SOURCES+=("$s") ;;
                *)     C_SOURCES+=("$s") ;;
            esac
        done
    else
        # Build a prune expression for the excluded private-lib directories.
        local -a PRUNE=()
        for d in "${LIB_EXCLUDE_DIRS[@]}"; do
            PRUNE+=(-path "$d" -prune -o)
        done
        # Never glob host-only test harnesses (test/ and tests/ dirs hold their
        # own main() and duplicate registry stubs -> "multiple definition" at
        # link time) or a vendored .git dir. These are compiled separately (see
        # tests/Makefile), never bundled into the FAP.
        PRUNE+=(-name tests -type d -prune -o)
        PRUNE+=(-name test -type d -prune -o)
        PRUNE+=(-name .git -type d -prune -o)
        # Prune nested app roots: any SUBdir that carries its own application.fam
        # is a separate app (ufbt "separate app root" marker), e.g. a vendored
        # PlatformIO/ESP32 port tagged "[DO NOT BUILD]". Its sources need a
        # foreign toolchain (Arduino.h, …) and must never land in this FAP.
        while IFS= read -r nested_fam; do
            PRUNE+=(-path "$(dirname "$nested_fam")" -prune -o)
        done < <(find "$APP_DIR" -mindepth 2 -name application.fam -type f)
        C_SOURCES=($(find "$APP_DIR" "${PRUNE[@]}" -name '*.c' -type f -print))
        CXX_SOURCES=($(find "$APP_DIR" "${PRUNE[@]}" -name '*.cpp' -type f -print))
        # Add the selected private-lib sources back in.
        for s in "${LIB_SOURCES[@]}"; do
            case "$s" in
                *.cpp) CXX_SOURCES+=("$s") ;;
                *)     C_SOURCES+=("$s") ;;
            esac
        done
    fi
    # Add generated icon .c files
    if [ -d "$ICONS_GEN_DIR" ]; then
        for icon_src in "$ICONS_GEN_DIR"/*.c; do
            [ -f "$icon_src" ] && C_SOURCES+=("$icon_src")
        done
    fi

    local TOTAL_SOURCES=$(( ${#C_SOURCES[@]} + ${#CXX_SOURCES[@]} ))
    echo "  Sources: $TOTAL_SOURCES files (${#C_SOURCES[@]} C, ${#CXX_SOURCES[@]} C++)"

    # Auto-discover private lib include paths under $APP_DIR/lib/<libname>/
    # (entspricht fap_private_libs[*].fap_include_paths in application.fam)
    local -a APP_INCLUDES=(
        -I"$APP_DIR" -I"$APP_DIR/helpers" -I"$APP_DIR/scenes"
        -I"$APP_DIR/views" -I"$APP_DIR/protocols"
        -I"$APP_DIR/app" -I"$APP_DIR/lib"
    )
    # Plugin source dirs hold nfc_supported_card_plugin.h etc.
    [ -n "$FAP_SINGLE_SOURCE" ] && APP_INCLUDES+=(-I"$(dirname "$FAP_SINGLE_SOURCE")")
    if [ -n "$FAP_SOURCES" ]; then
        for s in $FAP_SOURCES; do APP_INCLUDES+=(-I"$(dirname "$s")"); done
    fi
    # Extra include dirs (space-separated, project-relative or absolute)
    if [ -n "$FAP_EXTRA_INCLUDES" ]; then
        for inc in $FAP_EXTRA_INCLUDES; do
            case "$inc" in
                /*) APP_INCLUDES+=(-I"$inc") ;;
                *)  APP_INCLUDES+=(-I"$PROJECT_DIR/$inc") ;;
            esac
        done
    fi
    if [ -d "$APP_DIR/lib" ]; then
        for libdir in "$APP_DIR"/lib/*/; do
            [ -d "$libdir" ] && APP_INCLUDES+=(-I"${libdir%/}")
        done
    fi

    # Compile C sources
    local -a OBJECTS=()
    for src in "${C_SOURCES[@]}"; do
        local obj="$BUILD_DIR/$(echo "$src" | sed 's|/|_|g' | sed 's|\.c$|.o|')"
        OBJECTS+=("$obj")

        "$CC" "${COMMON_CFLAGS[@]}" "${TARGET_CFLAGS[@]}" \
            "${COMMON_INCLUDES[@]}" "${IDF_COMMON_INCLUDES[@]}" "${TARGET_INCLUDES[@]}" \
            "${APP_INCLUDES[@]}" \
            -c "$src" -o "$obj"
    done

    # Compile C++ sources
    for src in "${CXX_SOURCES[@]}"; do
        local obj="$BUILD_DIR/$(echo "$src" | sed 's|/|_|g' | sed 's|\.cpp$|.o|')"
        OBJECTS+=("$obj")

        "$CXX" "${COMMON_CXXFLAGS[@]}" "${TARGET_CFLAGS[@]}" \
            "${COMMON_INCLUDES[@]}" "${IDF_COMMON_INCLUDES[@]}" "${TARGET_INCLUDES[@]}" \
            "${APP_INCLUDES[@]}" \
            -c "$src" -o "$obj"
    done

    # Link
    echo "  Linking ${#OBJECTS[@]} objects (entry=$ENTRY_POINT)"
    "$LD" -r -T "$SCRIPT_DIR/tools/fap.ld" --entry="$ENTRY_POINT" -o "$BUILD_DIR/app.elf" "${OBJECTS[@]}"
    local SECTIONS=$("$READELF" -S "$BUILD_DIR/app.elf" | grep -c '^\s*\[')

    # Manifest with icon
    local ICON_ARG=""
    if [ -n "$APP_ICON" ] && [ -f "$APP_ICON" ]; then
        ICON_ARG="--icon $APP_ICON"
    fi
    python3 "$SCRIPT_DIR/tools/fap_manifest.py" \
        --name "$APP_NAME" \
        --api-major 1 --api-minor 0 \
        --target 32 \
        --stack-size "$APP_STACK" \
        --app-version 1 \
        $ICON_ARG \
        --output "$BUILD_DIR/manifest.bin"

    # Inject manifest into ELF + strip Debug-Sections.
    # --strip-debug entfernt .debug_* (DWARF) und ihre .rela.debug_* — bei Doom
    # ~4.3 MB von 4.7 MB. Symbol-Tabelle (.symtab/.strtab) und Code-Relocations
    # (.rela.text/.data/.rodata) bleiben erhalten, sind für ELF-Loader essentiell.
    # app.elf bleibt mit voller Debug-Info verfügbar für lokale Analyse.
    "$OBJCOPY" --add-section .fapmeta="$BUILD_DIR/manifest.bin" \
        --set-section-flags .fapmeta=contents,readonly \
        --strip-debug \
        "$BUILD_DIR/app.elf" "$OUTPUT"

    local SIZE=$(wc -c < "$OUTPUT")

    # Verify all symbols can be resolved by the firmware API table
    local API_FILE="$PROJECT_DIR/components/flipper_application/flipper_application/firmware_api.c"
    local NM="${TOOLCHAIN}-nm"
    "$NM" -u "$OUTPUT" 2>/dev/null | grep "^         U " | sed 's/^         U //' | sort -u > "$BUILD_DIR/undef_syms.txt"
    # Informational only: the .fal is already produced; missing symbols are
    # resolved by adding them to firmware_api.c (tools/add_symbol.py). Don't
    # let a non-zero exit abort the build under `set -e`.
    python3 "$PROJECT_DIR/tools/check_fap_symbols.py" "$API_FILE" "$BUILD_DIR/undef_syms.txt" || true

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

# ── Embedded plugins (fal_embedded) ─────────────────────────────────
# Apps like protopirate ship their protocol decoders as separate PLUGIN apps
# and load them at runtime from APP_ASSETS_PATH("plugins/<appid>.fal"). Build
# each as a .fal by re-invoking ourselves with the per-plugin sources/entry/
# cdefines. Skip while we ARE a plugin sub-build (FAP_SOURCES set) so we don't
# recurse. Output goes to <appid>_assets/plugins/ for deployment to the SD at
# /ext/apps_assets/<mainappid>/plugins/.
if [ -z "$FAP_SOURCES" ] && [ -z "$FAP_OUTPUT_NAME" ] && [ -f "$APP_DIR/application.fam" ]; then
    PLUGIN_INFO=$(python3 "$SCRIPT_DIR/tools/fap_app_info.py" "$APP_DIR" 2>/dev/null | grep '^PLUGIN' || true)
    if [ -n "$PLUGIN_INFO" ]; then
        echo ""
        echo "━━━ Embedded plugins (fal_embedded) ━━━"
        _p_id=""; _p_ep=""; _p_srcs=""; _p_defs=""
        build_one_plugin() {
            [ -z "$_p_id" ] && return 0
            echo ""
            echo "  ┄ plugin: $_p_id (entry=$_p_ep)"
            FAP_SOURCES="$_p_srcs" \
            FAP_ENTRY_OVERRIDE="$_p_ep" \
            FAP_STACK_OVERRIDE=0 \
            FAP_OUTPUT_NAME="${APP_ID}_assets/plugins/${_p_id}.fal" \
            FAP_CDEFINES="$_p_defs" \
            "$SCRIPT_DIR/buildFap.sh" "$APP_DIR" 2>&1 \
                | grep -E '✓|error:|fatal error:|missing API symbols' | sed 's/^/    /' || true
        }
        while IFS=$'\t' read -r kind a b; do
            case "$kind" in
                PLUGIN)
                    build_one_plugin              # flush the previous plugin
                    _p_id="$a"; _p_ep="$b"; _p_srcs=""; _p_defs="" ;;
                PLUGINSRC)  _p_srcs="$_p_srcs $APP_DIR/$b" ;;
                PLUGINDEF)  _p_defs="$_p_defs -D$b" ;;
            esac
        done <<< "$PLUGIN_INFO"
        build_one_plugin                          # flush the last plugin
    fi
fi

echo ""
echo "Done."

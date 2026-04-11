---
name: fix-fap-symbols
description: Fix missing FAP API symbols. Run after buildFap.sh reports missing symbols. Adds them to firmware_api.c, ports from STM32 firmware if needed, rebuilds and verifies.
argument-hint: [app_directory] (e.g. applications_user/protopirate_fap_test)
---

# Fix Missing FAP API Symbols

When `./buildFap.sh <app_dir>` reports missing API symbols, this skill resolves them.

## Context

- **FAP build script**: `./buildFap.sh`
- **API table**: `components/flipper_application/flipper_application/firmware_api.c`
- **Hash generator**: `python3 tools/gen_api_table.py <symbol_names...>`
- **Symbol checker**: `python3 tools/check_fap_symbols.py <firmware_api.c> <symbols_file>`
- **STM32 reference firmware**: `/Users/matthias/www/privat/flipper-zero-esp32-port/stm32/unleashed-firmware/`
- **ESP32 port**: `/Users/matthias/www/privat/flipper-zero-esp32-port_git/`

## Steps

### 1. Identify missing symbols

Run `./buildFap.sh $ARGUMENTS` and collect the missing symbol names from the output.

### 2. Classify each missing symbol

For each missing symbol, determine its type:

- **Firmware function** (e.g. `canvas_draw_rbox`, `furi_timer_start`): Already exists in ESP32 firmware, just not in the API table. Add hash entry.
- **Icon/asset** (e.g. `I_DolphinDone_80x58`): Global variable from `assets_icons.h`. Add hash entry with `&` prefix.
- **Notification sequence/message** (e.g. `sequence_success`, `message_vibro_on`): Global variable from `notification_messages.h`. Add hash entry with `&` prefix.
- **GCC runtime** (e.g. `__udivdi3`, `__bswapsi2`): Compiler helper from libgcc. Add `extern` declaration + hash entry.
- **Newlib** (e.g. `__assert_func`, `__getreent`): C library function. Already declared via `<sys/reent.h>` or similar.
- **Missing function** (not in ESP32 port): Needs to be **ported from STM32 firmware**.

### 3. Check if symbol exists in ESP32 firmware

```bash
# Search ESP32 port for the symbol
grep -rn "symbol_name" components/ lib/ --include="*.h" --include="*.c" | head -5

# If not found, search STM32 firmware
grep -rn "symbol_name" /Users/matthias/www/privat/flipper-zero-esp32-port/stm32/unleashed-firmware/ --include="*.h" --include="*.c" | head -5
```

If the function exists in STM32 but not ESP32, port it:
1. Find the source file in STM32
2. Copy or adapt the function to the ESP32 port
3. Ensure the header declares it

### 4. Generate hash entries

```bash
python3 tools/gen_api_table.py symbol1 symbol2 symbol3
```

### 5. Add to firmware_api.c

Insert the new entries into the **sorted** table in `firmware_api.c`. The table MUST remain sorted by hash value.

Rules for the `.address` field:
- Functions: `(uint32_t)function_name`
- Icons (`I_*`): `(uint32_t)&I_name`
- Notification messages (`message_*`): `(uint32_t)&message_name`
- Notification sequences (`sequence_*`): `(uint32_t)&sequence_name`

If new `#include` headers are needed, add them to the include block at the top of `firmware_api.c`.

### 6. Rebuild and verify

```bash
# Rebuild firmware
./buildAndFlash_T-Embed.sh --build-only

# Rebuild FAP and verify symbols
./buildFap.sh $ARGUMENTS
```

The build should show `✓ All N symbols resolved` for the ESP32-S3 target.

### 7. Regenerate full table (alternative bulk approach)

If many symbols are missing, regenerate the entire table from all FAPs:

```bash
. ~/esp/esp-idf/export.sh
# Collect all undefined symbols from all built FAPs
for fap in build_t_embed/fap/*.fap; do
    xtensa-esp32s3-elf-nm -u "$fap" | grep "^         U " | sed 's/^         U //'
done | sort -u > /tmp/all_syms.txt

# Generate sorted table
python3 tools/gen_api_table.py -f /tmp/all_syms.txt

# Then replace the table in firmware_api.c, fixing Icon/message/sequence references:
# - I_* symbols need & prefix
# - message_* symbols need & prefix  
# - sequence_* symbols need & prefix
```

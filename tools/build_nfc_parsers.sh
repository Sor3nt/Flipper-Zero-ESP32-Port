#!/bin/bash
#
# Build ALL NFC supported_cards parser plugins as native ESP32 (Xtensa) .fal
# files. The upstream Flipper ships these as ARM ELF objects which the ESP32
# FAP loader can't run — this rebuilds each one for the port's target.
#
# Enumerates every FlipperAppType.PLUGIN in applications/main/nfc/application.fam
# whose sources live under plugins/supported_cards/ (the card parsers; cli_nfc
# and other non-parser plugins are skipped), then drives buildFap.sh once per
# plugin via its FAP_* override hooks.
#
# Output:  build_t_embed/fap/<appid>.fal
# Symbols: aggregated missing-API-symbol report at the end (resolve with
#          tools/add_symbol.py, then rebuild firmware).
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NFC_APP_DIR="$PROJECT_DIR/applications/main/nfc"
FAM="$NFC_APP_DIR/application.fam"
OUT_DIR="$PROJECT_DIR/build_t_embed/fap"

# ── Enumerate parser plugins from application.fam ───────────────────
# Emits one line per plugin:  appid|entry_point|cdefines|src1,src2,...
PLUGINS=$(python3 - "$FAM" <<'PYEOF'
import ast, re, sys

src = open(sys.argv[1]).read()

# Grab every App( ... ) call body, balanced-paren aware.
calls = []
i = 0
while True:
    j = src.find("App(", i)
    if j < 0:
        break
    k = j + 3
    depth = 0
    while k < len(src):
        c = src[k]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                break
        k += 1
    calls.append(src[j + 4 : k])
    i = k + 1

def kv(body, key):
    m = re.search(rf'{key}\s*=\s*("([^"]*)"|\[[^\]]*\])', body)
    return m.group(1) if m else None

for body in calls:
    if "FlipperAppType.PLUGIN" not in body:
        continue
    sources_raw = kv(body, "sources")
    if not sources_raw or not sources_raw.startswith("["):
        continue
    sources = ast.literal_eval(sources_raw)
    if not any("plugins/supported_cards/" in s for s in sources):
        continue
    appid = ast.literal_eval(kv(body, "appid"))
    ep = ast.literal_eval(kv(body, "entry_point"))
    cdef = ""
    cdraw = kv(body, "cdefines")
    if cdraw and cdraw.startswith("["):
        for k_, v_ in ast.literal_eval(cdraw):
            cdef += f"-D{k_}={v_} "
    print(f"{appid}|{ep}|{cdef.strip()}|{','.join(sources)}")
PYEOF
)

TOTAL=$(echo "$PLUGINS" | grep -c .)
echo "Found $TOTAL parser plugins to build"
echo ""

FAILED=()
BUILT=0
MISSING_ALL="$OUT_DIR/.missing_symbols_all.txt"
mkdir -p "$OUT_DIR"
: > "$MISSING_ALL"

while IFS='|' read -r APPID EP CDEF SRCS; do
    [ -z "$APPID" ] && continue
    # Absolute, comma->space separated source list rooted at the nfc app dir.
    ABS_SRCS=""
    for s in ${SRCS//,/ }; do
        ABS_SRCS="$ABS_SRCS $NFC_APP_DIR/$s"
    done
    ABS_SRCS="${ABS_SRCS# }"

    # Some parsers use the NFC app's private api/ helpers. This port excludes
    # api/ from the firmware (application.fam: "!api"), so compile the needed
    # helper straight into the plugin to keep it self-contained.
    case "$APPID" in
        gallagher_parser)
            ABS_SRCS="$ABS_SRCS $NFC_APP_DIR/api/gallagher/gallagher_util.c" ;;
        troika_parser|social_moscow_parser)
            ABS_SRCS="$ABS_SRCS $NFC_APP_DIR/api/mosgortrans/mosgortrans_util.c" ;;
    esac

    echo "━━━ $APPID  (ep=$EP${CDEF:+, $CDEF}) ━━━"
    if FAP_SOURCES="$ABS_SRCS" \
       FAP_ENTRY_OVERRIDE="$EP" \
       FAP_CDEFINES="$CDEF" \
       FAP_STACK_OVERRIDE=0 \
       FAP_OUTPUT_NAME="${APPID}.fal" \
       FAP_EXTRA_INCLUDES="components/flipper_application/flipper_application components/datetime applications/main" \
       "$PROJECT_DIR/buildFap.sh" "$NFC_APP_DIR" > "$OUT_DIR/.build_${APPID}.log" 2>&1; then
        # Collect any missing-symbol report for the batch resolve step.
        grep -A100 'missing API symbols' "$OUT_DIR/.build_${APPID}.log" 2>/dev/null \
            | grep -oE '^\s+[a-zA-Z_][a-zA-Z0-9_]+ \(hash=' \
            | sed 's/ (hash=//' | tr -d ' ' >> "$MISSING_ALL" || true
        if [ -f "$OUT_DIR/${APPID}.fal" ]; then
            echo "  ✓ ${APPID}.fal"
            BUILT=$((BUILT + 1))
        else
            echo "  ✗ ${APPID}: no output (see .build_${APPID}.log)"
            FAILED+=("$APPID")
        fi
    else
        echo "  ✗ ${APPID}: build failed (see .build_${APPID}.log)"
        FAILED+=("$APPID")
    fi
done <<< "$PLUGINS"

echo ""
echo "═══════════════════════════════════════════════════"
echo "Built OK : $BUILT / $TOTAL"
echo "Failed   : ${#FAILED[@]} ${FAILED[*]}"
echo ""
if [ -s "$MISSING_ALL" ]; then
    sort -u "$MISSING_ALL" -o "$MISSING_ALL"
    echo "Missing API symbols (union, $(wc -l < "$MISSING_ALL")):"
    cat "$MISSING_ALL"
    echo ""
    echo "Resolve with:  python3 tools/add_symbol.py \$(cat $MISSING_ALL)"
else
    echo "No missing API symbols 🎉"
fi

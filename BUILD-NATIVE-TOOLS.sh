#!/usr/bin/env bash
# Build T-Embed Native Tools firmware and its SD applications; never flashes.
set -euo pipefail
cd "$(dirname "$0")"
: "${IDF_PATH:?Source ESP-IDF v5.4.1 export.sh first}"
python3 -c "from PIL import Image"
idf.py -B build_native -D SDKCONFIG=sdkconfig.wifite -D FLIPPER_BOARD=lilygo_t_embed_cc1101 build
for spec in rtl433:116 wifite:81 wardriver:152 wdgwars_map:13 bw16_r4tkn_i2c:45 protopirate:198147; do
  app="${spec%%:*}"
  version="${spec##*:}"
  FAP_FW_BUILD_DIR=build_native FAP_APP_VERSION_OVERRIDE="$version" bash buildFap.sh "applications_user/$app"
  app_id="$app"
  if [[ "$app" == protopirate ]]; then app_id=proto_pirate; fi
  python3 tools/check_fap_symbols.py components/flipper_application/flipper_application/firmware_api.c "build_native/fap/objects_${app_id}.fap/undef_syms.txt"
  expected_stack=16384
  case "$app" in
    rtl433) expected_stack=32768 ;;
  esac
  python3 tests/wdgwars_map/check_abi.py --firmware-build build_native --expected-stack "$expected_stack" "build_native/fap/${app_id}.fap"
done

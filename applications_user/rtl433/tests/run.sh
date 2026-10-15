#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out_dir="${RTL_TEST_OUT_DIR:-$(mktemp -d)}"
mkdir -p "$out_dir"
if [[ -z "${RTL_TEST_OUT_DIR:-}" ]]; then trap 'rm -rf "$out_dir"' EXIT; fi
gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/test_scan.c rtl_scan.c -o "$out_dir/scan"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/scan"
gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Itests/radio_stubs -I. -I../.. -I../../components/toolbox tests/test_radio.c rtl_radio.c rtl_scan.c rtl_capture.c rtl_file.c -pthread -o "$out_dir/radio"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/radio"
gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Itests/radio_stubs -I. -I../.. -I../../components/toolbox -Ilib/rtl433 tests/test_receive_decode.c rtl_radio.c rtl_scan.c rtl_capture.c rtl_file.c rtl_engine.c lib/rtl433/*.c lib/rtl433/devices/*.c -pthread -lm -o "$out_dir/receive_decode"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/receive_decode"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/receive_decode" backlog
gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. tests/test_screen.c ui/screen.c -o "$out_dir/screen"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/screen"
gcc -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ilib/rtl433 -I. tests/test_engine.c tests/test_fields.c tests/test_schedule.c tests/test_doorbell.c tests/test_catalog.c rtl_engine.c rtl_text.c rtl_capture.c rtl_file.c lib/rtl433/*.c lib/rtl433/devices/*.c -lm -o "$out_dir/test"
RTL_TEST_OUT="$out_dir" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/test"
python3 tests/check_fields.py "$out_dir"
gcc -std=gnu17 -g -O1 -fsanitize=address,undefined -fno-builtin -DESP_PLATFORM -Itests/stubs -D__floatdidf=rtl_floatdidf -D__floatdisf=rtl_floatdisf -Dllround=rtl_llround -Dsqrtf=rtl_sqrtf -Dsprintf=rtl_sprintf -Dstrtol=rtl_strtol -Dlocaltime_r=rtl_localtime_r -Dmktime=rtl_mktime -Dgettimeofday=rtl_gettimeofday -Dtime=rtl_time -Dperror=rtl_perror -Dstrftime=rtl_strftime -c rtl_compat.c -o "$out_dir/compat.o"
gcc -std=gnu17 -g -O1 -fsanitize=address,undefined -fno-builtin -Itests tests/test_compat.c "$out_dir/compat.o" -lm -o "$out_dir/compat"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$out_dir/compat"

#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cs_test_dir="$(mktemp -d)"
trap 'rm -rf "$cs_test_dir"' EXIT
gcc -std=gnu17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I. net_protocol.c tests/test_protocol.c -o "$cs_test_dir/protocol"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$cs_test_dir/protocol"
gcc -std=gnu17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Itests/stubs -I. -I../../components net_protocol.c net_job.c tests/test_jobs.c -lpthread -o "$cs_test_dir/jobs"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$cs_test_dir/jobs"

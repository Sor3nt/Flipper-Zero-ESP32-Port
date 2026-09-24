#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=build_host_bw16
mkdir -p "$out"
app=applications_user/bw16_r4tkn
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I"$app" \
    tests/bw16/test_protocol.c "$app/bw16_protocol.c" -o "$out/test_protocol"
"$out/test_protocol"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/bw16/stubs -I"$app" -Icomponents/furi_hal \
    tests/bw16/test_uart.c "$app/bw16_uart.c" -o "$out/test_uart"
"$out/test_uart"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/bw16/stubs -Icomponents/furi_hal '-DBOARD_INCLUDE="board_test.h"' \
    tests/bw16/test_pins.c components/furi_hal/furi_hal_shared_pins.c -o "$out/test_pins"
"$out/test_pins"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/bw16/stubs -Icomponents/furi_hal '-DBOARD_INCLUDE="board_test.h"' \
    tests/bw16/test_guard.c components/furi_hal/furi_hal_bw16_guard.c -o "$out/test_guard"
"$out/test_guard"

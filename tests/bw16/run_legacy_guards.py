"""Retained guarded-UART ABI regression using its original hardware mocks."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tests/host'))
from host_build import run
out = root/'build_feature_tests/legacy_guards'
out.mkdir(parents=True, exist_ok=True)
for test, source in [('pins','furi_hal_shared_pins.c'), ('guard','furi_hal_bw16_guard.c')]:
    exe = out/test
    run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g',
         '-fsanitize=address,undefined', '-DBOARD_INCLUDE="board_test.h"',
         '-I'+str(root/'tests/bw16/legacy_stubs'), '-I'+str(root/'tests/bw16/stubs'),
         '-I'+str(root/'components/furi_hal'), str(root/f'tests/bw16/test_{test}.c'),
         str(root/'components/furi_hal'/source), '-o', str(exe)], check=True)
    run([str(exe)], check=True)

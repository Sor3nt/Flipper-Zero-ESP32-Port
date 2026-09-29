"""Host-only portable BW16 capture checks; no radio or UART."""
from pathlib import Path
import os
import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests/host'))
from host_build import run
if os.name == 'nt' and not os.environ.get('ZIG_CC'):
    local_zig = root / '.backups/bw16-evidence/native/ziglang/zig.exe'
    if local_zig.is_file():
        os.environ['ZIG_CC'] = str(local_zig)
out = root / 'build_t_embed/host_bw16_pcap'
out.mkdir(parents=True, exist_ok=True)
core = root / 'firmware/bw16_controller'
exe = out / ('test_pcap.exe' if os.name == 'nt' else 'test_pcap')
run(['gcc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
     '-fsanitize=address,undefined', '-I' + str(core),
     str(root / 'tests/bw16_pcap/test_pcap.c'), str(core / 'bw16_pcap.c'),
     '-o', str(exe)], check=True)
run([str(exe)], check=True)

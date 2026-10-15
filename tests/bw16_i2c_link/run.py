"""Host-only checks for the shared I2C link; never opens I2C, UART or radio."""
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
out = root / 'build_t_embed/host_bw16_i2c_link'
out.mkdir(parents=True, exist_ok=True)
link = root / 'firmware/bw16_i2c_link'
pcap = root / 'firmware/bw16_controller'
exe = out / ('test_i2c_link.exe' if os.name == 'nt' else 'test_i2c_link')
run(['gcc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
     '-fsanitize=address,undefined', '-I' + str(link), '-I' + str(pcap),
     str(root / 'tests/bw16_i2c_link/test_i2c_link.c'),
     str(link / 'bw16_i2c_link.c'), str(pcap / 'bw16_pcap.c'),
     '-o', str(exe)], check=True)
run([str(exe)], check=True)

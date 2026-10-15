"""Exercise production boot ordering with failed/successful SD and legacy profiles."""
from pathlib import Path
import os,sys,tempfile
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
if os.name=='nt':
    os.environ.setdefault('ZIG_CC',str(root/'.backups/bw16-evidence/native/ziglang/zig.exe'))
out=Path(tempfile.mkdtemp(prefix='plus_sd_startup_',dir=root/'.backups'))
source=(root/'components/furi_hal/furi_hal.c').read_text()
source=source[source.index('bool furi_hal_external_uart_available'):]
fixture=(root/'tests/plus_sd_recovery/test_startup.c').read_text()
assert fixture.count('/* PRODUCTION */')==1
src=out/'startup.c'
src.write_text(fixture.replace('/* PRODUCTION */',source))
for recovery,uart in ((1,0),(0,0),(0,1)):
    exe=out/f'startup_{recovery}_{uart}.exe'
    run(['gcc','-std=c17','-O1','-g','-fsanitize=address,undefined',
         f'-DCONFIG_T_EMBED_PLUS_SD_RECOVERY={recovery}',
         f'-DCONFIG_T_EMBED_EXTERNAL_UART={uart}',
         str(src),'-o',str(exe)],check=True)
    run([str(exe)],check=True)
print('PASS: Plus SD-before-SPI boot, no-card continuation, inactive chip selects and unchanged legacy GPIO profiles')

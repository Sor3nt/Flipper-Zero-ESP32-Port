"""Run real protocol, control, UART/GPS and UI code against host boundaries."""
from pathlib import Path
import os, sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
os.chdir(root)
out=root/'build_t_embed/host_bw16'
out.mkdir(parents=True,exist_ok=True)
app=root/'applications_user/bw16_r4tkn'
gps=root/'applications_user/wardriver'
for name in ('protocol','control','selection','batch','uart'):
    sources=[app/f'bw16_{name}.c']
    if name in ('control','selection'): sources+=[app/'bw16_protocol.c']
    if name=='batch': sources+=[app/'bw16_control.c',app/'bw16_protocol.c']
    if name=='uart': sources+=[gps/'wardriver_gps.c',gps/'wardriver_nmea.c']
    exe=out/f'test_{name}'
    run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g',
         '-fsanitize=address,undefined','-I'+str(root/'tests/bw16/stubs'),
         '-I'+str(app),'-I'+str(gps),'-I'+str(root/'components/furi_hal'),str(root/f'tests/bw16/test_{name}.c'),
         *map(str,sources),'-lm','-o',str(exe)],check=True)
    run([str(exe)],check=True)
run([sys.executable,str(root/'tests/bw16/test_profile.py')],check=True)
run([sys.executable,str(root/'tests/bw16/test_ui.py')],check=True)

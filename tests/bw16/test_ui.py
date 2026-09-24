"""Compile the real app's entry, callbacks and renderer against OS/UART mocks."""
from pathlib import Path
import re
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
out=root/'build_host_bw16'
out.mkdir(exist_ok=True)
app=root/'applications_user/bw16_r4tkn'
u8g2=root/'components/u8g2'
sources=[u8g2/n for n in re.findall(r'"([^"]+\.c)"',(u8g2/'CMakeLists.txt').read_text())]
exe=out/'test_ui'
run(['gcc','-std=gnu17','-O1','-g','-ffunction-sections','-fdata-sections',
     '-I'+str(root/'tests/bw16/ui_stubs'),'-I'+str(root/'tests/bw16/stubs'),
     '-I'+str(app),'-I'+str(root/'components/furi_hal'),'-I'+str(u8g2),
     str(root/'tests/bw16/test_ui.c'),str(app/'bw16_control.c'),str(app/'bw16_protocol.c'),
     *map(str,sources),'-Wl,--gc-sections','-o',str(exe)],check=True)
run([str(exe),str(out/'controls-ui.pgm')],check=True)

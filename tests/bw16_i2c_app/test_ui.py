"""Compile the real app's entry, callbacks and renderer against OS/I2C transport mocks."""
from pathlib import Path
import re
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
out=root/'build_host_bw16_i2c'
out.mkdir(exist_ok=True)
app=root/'applications_user/bw16_r4tkn_i2c'
u8g2=root/'components/u8g2'
sources=[u8g2/n for n in re.findall(r'"([^"]+\.c)"',(u8g2/'CMakeLists.txt').read_text())]
exe=out/'test_ui'
run(['gcc','-std=gnu17','-O1','-g','-ffunction-sections','-fdata-sections',
     '-I'+str(root/'tests/bw16_i2c_app/ui_stubs'),
     '-I'+str(app),'-I'+str(root/'components/furi_hal'),'-I'+str(u8g2),
     str(root/'tests/bw16_i2c_app/test_ui.c'),str(app/'bw16_control.c'),str(app/'bw16_protocol.c'),
     str(app/'bw16_selection.c'),str(app/'bw16_batch.c'),
     str(app/'bw16_capture.c'),str(app/'bw16_pcap.c'),
     *map(str,sources),'-Wl,--gc-sections','-o',str(exe)],check=True)
run([str(exe),str(out/'controls-ui.pgm')],check=True)

linked=out/'test_ui_linked'
run(['gcc','-std=gnu17','-O1','-g','-DBW16_REAL_I2C','-ffunction-sections','-fdata-sections',
     '-I'+str(root/'tests/bw16_i2c_app/ui_stubs'),'-I'+str(app),
     '-I'+str(root/'components/furi_hal'),'-I'+str(u8g2),
     str(root/'tests/bw16_i2c_app/test_ui.c'),str(app/'bw16_control.c'),str(app/'bw16_protocol.c'),
     str(app/'bw16_selection.c'),str(app/'bw16_batch.c'),str(app/'bw16_capture.c'),str(app/'bw16_pcap.c'),
     str(app/'bw16_i2c.c'),str(app/'bw16_i2c_codec.c'),
     *map(str,sources),'-Wl,--gc-sections','-o',str(linked)],check=True)
run([str(linked)],check=True)

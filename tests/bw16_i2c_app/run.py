"""Exercise the isolated I2C adapter, app, SD boundaries and UART preservation."""
from pathlib import Path
import hashlib, json, os, sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
os.chdir(root)
for name, expected in json.loads((root/'tests/bw16_i2c_app/uart_baseline_sha256.json').read_text()).items():
    assert hashlib.sha256((root/name).read_bytes().replace(b'\r\n', b'\n')).hexdigest()==expected, 'UART baseline changed: '+name
print('PASS: existing UART app and tests remain byte-identical',flush=True)
app=root/'applications_user/bw16_r4tkn_i2c'
out=root/'build_host_bw16_i2c';out.mkdir(exist_ok=True)
exe=out/'test_transport'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
     '-I'+str(root/'tests/bw16_i2c_app/ui_stubs'),'-I'+str(app),
     str(root/'tests/bw16_i2c_app/test_transport.c'),str(app/'bw16_i2c.c'),
     str(app/'bw16_i2c_codec.c'),str(app/'bw16_control.c'),str(app/'bw16_protocol.c'),
     '-o',str(exe)],check=True)
run([str(exe)],check=True)
run([sys.executable,str(root/'tests/bw16_i2c_app/test_ui.py')],check=True)

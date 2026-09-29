"""Check Plus-only saved GPS pin isolation and unchanged survey behavior."""
from pathlib import Path
import hashlib,json,sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
app=root/'applications_user/wardriver_plus_i2c'
out=root/'build_t_embed_plus_i2c/host_wardriver';out.mkdir(parents=True,exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
for name,digest in json.loads((root/'tests/wardriver_plus_i2c/original_sha256.json').read_text()).items():
    assert sha(root/name)==digest,'Original changed: '+name
allowed={'application.fam','wardriver_gps.c','wardriver.c','README.md'}
for f in (root/'applications_user/wardriver').iterdir():
    if f.is_file() and f.name not in allowed:assert sha(f)==sha(app/f.name),'Unexpected change: '+f.name
print('PASS: original Wardriver sources/FAPs intact; config, Wi-Fi, BLE, detection and survey modules byte-identical',flush=True)
for profile in (0,1):
    binary=out/f'gps_profile_{profile}'
    run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
         f'-DCONFIG_T_EMBED_BW16_I2C={profile}','-I'+str(root/'tests/wardriver_plus_i2c/stubs'),
         '-I'+str(root/'tests/host/wardriver_host'),'-I'+str(root/'tests/bw16/stubs'),'-I'+str(app),
         str(root/'tests/wardriver_plus_i2c/test_reserved_gps.c'),str(app/'wardriver_gps.c'),
         str(app/'wardriver_config.c'),str(app/'wardriver_nmea.c'),'-lm','-o',str(binary)],check=True)
    run([str(binary)],check=True)
radio_source=(app/'wardriver_wifi.c').read_text()
radio_source='\n'.join(line for line in radio_source.splitlines() if not line.startswith('#include'))
code=(root/'tests/host/wardriver_radio_backend.h').read_text()+radio_source+(root/'tests/host/wardriver_radio_test.c').read_text()
test=out/'radio.c';test.write_text(code);binary=out/'radio'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
     '-I'+str(root/'tests/host/wardriver_host'),'-I'+str(app),str(test),
     str(app/'wardriver_detect.c'),str(app/'wardriver_monitor.c'),'-o',str(binary)],check=True)
run([str(binary)],check=True)
print('PASS: unchanged Wi-Fi/BLE radio lifecycle, no dependency on GPS initialization',flush=True)

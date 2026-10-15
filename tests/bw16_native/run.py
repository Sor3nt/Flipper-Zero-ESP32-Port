"""Host-only BW16 firmware core and real FAP protocol integration tests."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
out=root/'build_t_embed/host_bw16_native'
out.mkdir(parents=True,exist_ok=True)
core=root/'firmware/bw16_controller'
app=root/'applications_user/bw16_r4tkn'
exe=out/'test_native'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
    '-I'+str(core),'-I'+str(app),str(root/'tests/bw16_native/test_native.c'),
    str(core/'bw16_native.c'),str(core/'bw16_pcap.c'),str(app/'bw16_control.c'),str(app/'bw16_protocol.c'),'-o',str(exe)],check=True)
run([str(exe)],check=True)

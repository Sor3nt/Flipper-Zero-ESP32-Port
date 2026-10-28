from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
out=root/'build_t_embed/host_bw16_capture_adapter';out.mkdir(parents=True,exist_ok=True)
core=root/'firmware/bw16_controller'
for name in ('bw16_native','bw16_pcap'):
 run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-I'+str(core),'-c',str(core/(name+'.c')),'-o',str(out/(name+'.o'))],check=True)
exe=out/'test_adapter'
run(['gcc','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-I'+str(root/'tests/bw16_capture_adapter/stubs'),'-I'+str(core),str(root/'tests/bw16_capture_adapter/test_adapter.cpp'),str(out/'bw16_native.o'),str(out/'bw16_pcap.o'),'-o',str(exe)],check=True)
run([str(exe)],check=True)

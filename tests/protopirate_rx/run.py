"""Run production history lifetime/concurrency regressions with host storage doubles."""
from pathlib import Path
import os,re,sys,tempfile,subprocess
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
(ROOT/'.backups').mkdir(exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='protopirate_rx_',dir=ROOT/'.backups'))
source=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'applications_user/protopirate/protopirate_history.c'
strip=lambda s:'\n'.join(l for l in s.splitlines() if not re.match(r'\s*#include\b',l))+'\n'
(out/'history_under_test.h').write_text(strip((ROOT/'applications_user/protopirate/protopirate_history.h').read_text()))
(out/'history_under_test.inc').write_text(strip(source.read_text()))
exe=out/('history.exe' if os.name=='nt' else 'history')
run(['gcc','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g',
     '-fsanitize=address,undefined','-I'+str(out),'-I'+str(ROOT/'components/mlib'),
     str(ROOT/'tests/protopirate_rx/history_test.c'),'-o',str(exe)],check=True,cwd=ROOT)
run([exe],check=True,cwd=ROOT,timeout=40)
print('PASS production ProtoPirate history tests',flush=True)

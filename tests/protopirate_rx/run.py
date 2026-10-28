"""Run production history lifetime/concurrency regressions with host storage doubles.

Linux/macOS: python3 tests/protopirate_rx/run.py (GCC, ASan and UBSan).
Windows: set ZIG_CC to zig.exe, then run the same command (native mocks).
An optional source path runs the identical test against a pre-fix history.c.
"""
from pathlib import Path
import os,re,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    raise SystemExit('Set ZIG_CC to your installed zig.exe before running Windows host tests.')
source=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'applications_user/protopirate/protopirate_history.c'
strip=lambda s:'\n'.join(l for l in s.splitlines() if not re.match(r'\s*#include\b',l))+'\n'
with tempfile.TemporaryDirectory(prefix='protopirate_rx_') as directory:
    out=Path(directory)
    (out/'history_under_test.h').write_text(strip((ROOT/'applications_user/protopirate/protopirate_history.h').read_text()))
    (out/'history_under_test.inc').write_text(strip(source.read_text()))
    exe=out/('history.exe' if os.name=='nt' else 'history')
    run(['gcc','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-g',
         '-fsanitize=address,undefined','-pthread','-I'+str(out),'-I'+str(ROOT/'components/mlib'),
         str(ROOT/'tests/protopirate_rx/history_test.c'),'-o',str(exe)],check=True,cwd=ROOT)
    run([exe],check=True,cwd=ROOT,timeout=40)
print('PASS production ProtoPirate history tests',flush=True)

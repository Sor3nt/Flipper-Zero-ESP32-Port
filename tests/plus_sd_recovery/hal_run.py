"""Execute production SD HAL with fault-injected IDF boundaries, flag on/off."""
from pathlib import Path
import os,re,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
out=Path(tempfile.mkdtemp(prefix='plus_sd_hal_',dir=ROOT/'.backups'))
# Use complete production bodies; replace platform includes only with test doubles.
strip=lambda s:'\n'.join(l for l in s.splitlines() if not re.match(r'\s*#include\b',l))+'\n'
source=strip((ROOT/'components/furi_hal/furi_hal_sd.c').read_text())
(out/'furi_hal_sd_under_test.inc').write_text(source)
header=strip((ROOT/'components/furi_hal/furi_hal_sd.h').read_text())
(out/'furi_hal_sd_under_test.h').write_text('typedef enum {FuriStatusOk=0,FuriStatusError=-1} FuriStatus;\n'+header)
baseline=ROOT/'.backups/plus-sd-recovery-1.3.1/source-before/components/furi_hal/furi_hal_sd.c'
if baseline.exists():
    (out/'furi_hal_sd_baseline.inc').write_text(strip(baseline.read_text()))
    expanded=[]
    for kind,name in (("baseline","furi_hal_sd_baseline.inc"),("current","furi_hal_sd_under_test.inc")):
        unit=out/f'compare_{kind}.c'
        unit.write_text('#include "hal_stubs.h"\n#include "'+name+'"\n')
        result=run(['gcc','-E','-P','-DCONFIG_T_EMBED_PLUS_SD_RECOVERY=0','-I'+str(out),
                    '-I'+str(ROOT/'tests/plus_sd_recovery'),str(unit)],check=True,cwd=ROOT,capture_output=True,text=True)
        expanded.append(re.sub(r'\s+','',result.stdout))
    assert expanded[0]==expanded[1], 'Default-off HAL changed from source-before baseline'
    print('PASS: default-off preprocessed production HAL matches saved baseline exactly',flush=True)
for flag in (0,1):
    exe=out/(f'hal_{flag}.exe' if os.name=='nt' else f'hal_{flag}')
    run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
         f'-DCONFIG_T_EMBED_PLUS_SD_RECOVERY={flag}','-I'+str(out),'-I'+str(ROOT/'tests/plus_sd_recovery'),
         str(ROOT/'tests/plus_sd_recovery/test_hal.c'),'-o',str(exe)],check=True,cwd=ROOT)
    run([exe],check=True,cwd=ROOT)
print('PASS: SD HAL profile on/off production recovery regressions')

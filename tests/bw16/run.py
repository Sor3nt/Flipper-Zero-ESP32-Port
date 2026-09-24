"""Host tests: python tests/bw16/run.py [--zig path/to/zig.exe]."""
import argparse, os, pathlib, shutil, subprocess
p=argparse.ArgumentParser()
p.add_argument('--zig')
a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2]
os.chdir(root)
out=root/'build_host_bw16'
out.mkdir(exist_ok=True)
env=os.environ.copy()
env['ZIG_GLOBAL_CACHE_DIR']=str(out/'zig-cache')
compiler=[a.zig,'cc'] if a.zig else [shutil.which('cc') or 'cc']
app=root/'applications_user/bw16_r4tkn'
for name in ['protocol','uart','pins','guard']:
    exe=out/('test_'+name+('.exe' if os.name=='nt' else ''))
    args=compiler+['-std=c11','-Wall','-Wextra','-Werror','-g','-I'+str(app)]
    if os.name!='nt': args+=['-fsanitize=address,undefined']
    if name in ['uart','pins','guard']: args+=['-Itests/bw16/stubs','-Icomponents/furi_hal']
    source=app/f'bw16_{name}.c'
    if name in ['pins','guard']:
        args+=['-DBOARD_INCLUDE="board_test.h"']
        source=root/('components/furi_hal/furi_hal_shared_pins.c' if name=='pins' else 'components/furi_hal/furi_hal_bw16_guard.c')
    subprocess.run(args+[f'tests/bw16/test_{name}.c',str(source),'-o',str(exe)],env=env,check=True)
    subprocess.run([str(exe)],check=True)

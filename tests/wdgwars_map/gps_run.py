"""Compile the actual map GPS/config reuse units in all three hardware profiles."""
from pathlib import Path
import importlib.util, os, shutil, tempfile, time
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('host_build',ROOT/'tests/host/host_build.py')
host=importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
def cleanup_build(folder, temporary_root):
    # Antivirus/indexing can hold a just-exited EXE briefly on Windows. Delete
    # only the exact private directory created below, never a searched path.
    last_error=None
    for attempt in range(8):
        try:
            if not folder.exists(): return
            if folder.is_symlink() or folder.resolve()!=folder or folder.parent!=temporary_root or not folder.name.startswith('wdg_gps_'):
                print(f'WARNING: temporary cleanup refused unexpected path: {folder}',flush=True)
                return
            shutil.rmtree(folder)
            return
        except OSError as error:
            last_error=error
            if attempt<7: time.sleep(0.25*(attempt+1))
    # A cleanup lock is separate from the compiler/assertion result. This
    # finally block never replaces a failing test exception with cleanup noise.
    print(f'WARNING: temporary build files remain after 8 cleanup attempts: {folder} ({last_error})',flush=True)

temporary_root=Path(tempfile.gettempdir()).resolve()
d=Path(tempfile.mkdtemp(prefix='wdg_gps_',dir=temporary_root)).resolve()
try:
    for profile,i2c,uart in (("uart",0,1),("plus",1,0),("uart_i2c",1,1)):
        exe=Path(d)/(f'gps_{profile}.exe' if os.name=='nt' else f'gps_{profile}')
        includes=['tests/wardriver_plus_i2c/stubs','tests/host/wardriver_host','tests/bw16/stubs','applications_user/wardriver_plus_i2c']
        sources=['tests/wardriver_plus_i2c/test_reserved_gps.c','applications_user/wdgwars_map/reuse_wardriver_gps.c','applications_user/wdgwars_map/reuse_wardriver_config.c','applications_user/wdgwars_map/reuse_wardriver_nmea.c']
        host.run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',f'-DCONFIG_T_EMBED_BW16_I2C={i2c}',f'-DCONFIG_T_EMBED_EXTERNAL_UART={uart}',*['-I'+str(ROOT/p) for p in includes],*[ROOT/p for p in sources],'-lm','-o',exe],check=True,cwd=ROOT)
        host.run([exe],check=True,cwd=ROOT)
finally:
    cleanup_build(d,temporary_root)
print('PASS: WDGWars GPS UART, Plus I2C, and UART+I2C profiles; shared pins protected without disabling UART43/44')

"""Check raw USB handoff serializes cancellation of a pending SD remount."""
from pathlib import Path
import os,sys,tempfile
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
if os.name=='nt': os.environ.setdefault('ZIG_CC',str(root/'.backups/bw16-evidence/native/ziglang/zig.exe'))
out=Path(tempfile.mkdtemp(prefix='plus_sd_usb_',dir=root/'.backups'))
source=(root/'applications/services/desktop/scenes/desktop_scene_usb_storage.c').read_text()
source=source[source.index('static bool desktop_usb_storage_enter('):source.index('static void desktop_usb_storage_leave(')]
fixture=(root/'tests/plus_sd_recovery/test_usb_handoff.c').read_text()
src=out/'handoff.c';src.write_text(fixture.replace('/* PRODUCTION */',source))
for flag in (0,1):
 exe=out/f'usb_{flag}.exe'
 run(['gcc','-std=c17','-O1','-g','-fsanitize=address,undefined',f'-DCONFIG_T_EMBED_PLUS_SD_RECOVERY={flag}',str(src),'-o',str(exe)],check=True)
 run([str(exe)],check=True)
print('PASS: pending-recovery cancellation precedes USB raw access; busy/detach-failure prevents handoff; old profile retained')

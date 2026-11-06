"""Check USB-Storage exit: warn while the host locks the medium, then leave in order."""
from pathlib import Path
import os,sys,tempfile
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
if os.name=='nt': os.environ.setdefault('ZIG_CC',str(root/'.backups/bw16-evidence/native/ziglang/zig.exe'))
(root/'.backups').mkdir(exist_ok=True)
out=Path(tempfile.mkdtemp(prefix='plus_sd_usb_exit_',dir=root/'.backups'))
source=(root/'applications/services/desktop/scenes/desktop_scene_usb_storage.c').read_text()
source=source[source.index('static void desktop_usb_storage_leave('):source.index('#endif /* USB_STORAGE_HAVE_USB */')]
fixture=(root/'tests/plus_sd_recovery/test_usb_exit.c').read_text()
src=out/'exit.c';src.write_text(fixture.replace('/* PRODUCTION */',source))
exe=out/'usb_exit.exe'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
run([str(exe)],check=True)
print('PASS: USB-Storage exit guard and leave sequence')

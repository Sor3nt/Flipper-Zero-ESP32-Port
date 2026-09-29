"""Compile the exact SD mount/tick/service functions with mocked dependencies.

Unrelated filesystem IO and GUI allocation are omitted from the host translation unit;
recovery, manual mount/unmount/format, status events and the service loop are unmodified.
Every run writes to a fresh workspace directory, never a preserved firmware build.
"""
from pathlib import Path
import os, sys, tempfile
R=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(R/'tests/host'))
from host_build import run
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(R/'.backups/bw16-evidence/native/ziglang/zig.exe')
B=Path(tempfile.mkdtemp(prefix='plus_sd_service_',dir=R/'.backups'))
ext=(R/'applications/services/storage/storages/storage_ext.c').read_text()
svc=(R/'applications/services/storage/storage.c').read_text()
def function(text,signature):
    start=text.index(signature)
    pos=text.index('{',start)
    depth=1
    end=pos+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[start:end]+'\n'
preamble=ext[ext.index('typedef struct {'):ext.index('/******************* Core Functions')]
parts=[preamble]
for sig in ('static bool sd_mount_card_internal(', 'FS_Error sd_unmount_card(',
            'FS_Error sd_mount_card(', 'FS_Error sd_format_card(',
            'static void storage_ext_tick_internal(', 'static void storage_ext_tick(',
            'static FS_Error storage_ext_parse_error(SDError error) {',
            'void storage_ext_init('):
    parts.append(function(ext,sig))
parts.extend(function(svc,sig) for sig in ('void storage_tick(', 'int32_t storage_srv('))
processing=(R/'applications/services/storage/storage_processing.c').read_text()
parts.append(function(processing,'static FS_Error storage_process_sd_unmount('))
(B/'service_production.inc').write_text('\n'.join(parts))
for enabled in (0,1):
    exe=B/f'service_{enabled}.exe'
    run(['gcc','-std=c17','-Wall','-Wextra','-Werror',
         f'-DCONFIG_T_EMBED_PLUS_SD_RECOVERY={enabled}',
         '-I',R/'tests/plus_sd_recovery','-I',B,
         R/'tests/plus_sd_recovery/test_service.c','-o',exe],check=True)
    run([exe],check=True)
print('PASS: production SD service recovery and original-profile behavior; outputs:',B)

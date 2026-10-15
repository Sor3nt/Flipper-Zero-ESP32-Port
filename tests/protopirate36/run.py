"""Fault-injection checks against the actual port's loader and model reader."""
from pathlib import Path
import os, re, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/host'))
from host_build import run
APP = ROOT/'applications_user/protopirate'
def strip(text):
    return '\n'.join(line for line in text.splitlines() if not re.match(r'\s*#(?:include|pragma)\b', line))
with tempfile.TemporaryDirectory(prefix='pp36_') as tmp:
    tmp = Path(tmp)
    header = (APP/'helpers/protopirate_plugins.h').read_text()
    constants = '\n'.join(re.findall(r'^#define (?:\w*PLUGIN_(?:PATH|APP_ID|API_VERSION)|\w+_PLUGIN_PATH)\s+[^\n]+', header, re.M))
    for path in (APP/'scenes/plugins').glob('*.h'):
        constants += '\n'+'\n'.join(re.findall(r'^#define PROTOPIRATE_\w*PLUGIN_(?:APP_ID|API_VERSION)\s+[^\n]+',path.read_text(),re.M))
    path = APP/'protocols/protopirate_protocol_plugins.h'
    constants += '\n'+'\n'.join(re.findall(r'^#define PROTOPIRATE_\w*PLUGIN_(?:APP_ID|API_VERSION)\s+[^\n]+',path.read_text(),re.M))
    enum = re.search(r'typedef enum ProtoPirateSharedPlugin\s*\{.*?} ProtoPirateSharedPlugin;',header,re.S).group()
    loader = strip((APP/'helpers/protopirate_plugins.c').read_text()).split('bool shared_plugin_handle_navigation_events(')[0]
    (tmp/'loader.inc').write_text(constants+'\n'+enum+'\n'+loader)
    (tmp/'models.inc').write_text(strip((APP/'helpers/protopirate_models.c').read_text()))
    (tmp/'format.inc').write_text(strip((APP/'helpers/protopirate_format_compat.h').read_text()))
    for name in ('loader','models','format'):
        exe = tmp/(name+'.exe' if os.name == 'nt' else name)
        run(['gcc','-std=gnu17','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
             '-O1','-g','-fsanitize=address,undefined','-I'+str(tmp),
             str(ROOT/f'tests/protopirate36/{name}_test.c'),'-o',exe],check=True)
        run([exe],check=True,timeout=30)
print('PASS ProtoPirate 3.6.1 fault-injection checks')

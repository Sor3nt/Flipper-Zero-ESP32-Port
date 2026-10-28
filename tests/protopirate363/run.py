"""Exercise Configuration teardown with the production dispatcher functions."""
from pathlib import Path
import os, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/host'))
from host_build import run
if os.name == 'nt':
    os.environ.setdefault('ZIG_CC', str(ROOT / '.backups/bw16-evidence/native/ziglang/zig.exe'))
def function(path, signature):
    source = path.read_text(); start = source.index(signature)
    opening = source.index('{', start); depth = 1; end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end] + '\n'
with tempfile.TemporaryDirectory(prefix='pp363_') as tmp:
    tmp = Path(tmp); dispatcher = ROOT / 'components/gui/view_dispatcher.c'
    code = ''.join(function(dispatcher, signature) for signature in (
        'void view_dispatcher_stop(', 'void view_dispatcher_set_current_view(',
        'void view_dispatcher_switch_to_view(', 'void view_dispatcher_remove_view('))
    code += function(ROOT / 'applications_user/protopirate/scenes/protopirate_scene_receiver_config.c',
                     'void protopirate_scene_receiver_config_on_exit(')
    (tmp / 'production.inc').write_text(code)
    exe = tmp / ('config_back.exe' if os.name == 'nt' else 'config_back')
    run(['gcc', '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-O1', '-fsanitize=address,undefined', '-I' + str(tmp),
         ROOT / 'tests/protopirate363/config_back_test.c', '-o', exe], check=True)
    run([exe], check=True, timeout=30)

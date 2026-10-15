from pathlib import Path
import importlib.util, os, tempfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('host_build',ROOT/'tests/host/host_build.py')
host=importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
with tempfile.TemporaryDirectory(prefix='wdg_http_') as d:
    exe=Path(d)/('http.exe' if os.name=='nt' else 'http')
    host.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-O1','-g','-fsanitize=address,undefined','-I',ROOT/'tests/wdgwars_map/stubs','-I',ROOT/'applications_user/wdgwars_map',ROOT/'tests/wdgwars_map/test_client.c',ROOT/'applications_user/wdgwars_map/wdgwars_map_client.c',ROOT/'applications_user/wdgwars_map/wdgwars_map_json.c','-lm','-o',exe],check=True,cwd=ROOT)
    host.run([exe],check=True,cwd=ROOT)

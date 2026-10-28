from pathlib import Path
import importlib.util, os, tempfile, sys
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('host_build',ROOT/'tests/host/host_build.py')
host=importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
with tempfile.TemporaryDirectory(prefix='wdg_cache_') as d:
    exe=Path(d)/('cache.exe' if os.name=='nt' else 'cache')
    sources=['tests/wdgwars_map/cache_test.c','applications_user/wdgwars_map/wdgwars_map_cache.c','applications_user/wdgwars_map/wdgwars_map_cache_codec.c','applications_user/wdgwars_map/wdgwars_map_json.c','applications_user/wdgwars_map/wdgwars_map_basemap.c']
    host.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-O1','-g','-fsanitize=address,undefined','-I',ROOT/'tests/host/wardriver_host','-I',ROOT/'applications_user/wdgwars_map',*[ROOT/s for s in sources],'-lm','-o',exe],check=True,cwd=ROOT)
    host.run([exe]+sys.argv[1:],check=True,cwd=ROOT)

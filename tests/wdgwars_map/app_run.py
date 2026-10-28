from pathlib import Path
import importlib.util, os, tempfile, re
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('host_build',ROOT/'tests/host/host_build.py')
host=importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
source=(ROOT/'applications_user/wdgwars_map/wdgwars_map_app.c').read_text()
def function(name):
    match=re.search(r'^static [^\n]+\b'+re.escape(name)+r'\([^\n]*\) \{',source,re.M)
    assert match,name
    opening=source.index('{',match.start()); depth=1; end=opening+1
    while depth:
        if source[end]=='{': depth+=1
        elif source[end]=='}': depth-=1
        end+=1
    return source[match.start():end]
types=source[source.index('typedef enum { JobLoad'):source.index('static uint64_t uptime_ms')]
# Compile actual production helpers and worker bodies; mock only hardware,
# transport adapters and cache I/O. Projection functions are the real renderer.
names=['uptime_ms','now_ms','message','cancelled','epoch','has_geometry','view_bounds','bounds_cover','streets_cover','territories_cover','start_job','finish_job','view_changed','automatic_update','gps_startup','gps_toggle','coordinates_begin','coordinate_input','action','handle_input','backoff','worker_main']
helpers=types+'\nstatic int32_t worker_main(void*);\n'+'\n'.join(function(n) for n in names)
with tempfile.TemporaryDirectory(prefix='wdg_app_') as d:
    folder=Path(d); (folder/'app_helpers.inc').write_text(helpers)
    sources=['tests/wdgwars_map/test_app.c','applications_user/wdgwars_map/wdgwars_map_render.c','applications_user/wdgwars_map/wdgwars_map_world.c','applications_user/wdgwars_map/wdgwars_map_json.c']
    for profile,i2c,uart in (("uart",0,1),("plus",1,0),("uart_i2c",1,1)):
        exe=folder/(f'app_{profile}.exe' if os.name=='nt' else f'app_{profile}')
        host.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-O1','-g','-fsanitize=address,undefined',f'-DCONFIG_T_EMBED_BW16_I2C={i2c}',f'-DCONFIG_T_EMBED_EXTERNAL_UART={uart}','-I',folder,'-I',ROOT/'tests/wardriver_plus_i2c/stubs','-I',ROOT/'applications_user/wdgwars_map',*[ROOT/s for s in sources],'-lm','-o',exe],check=True,cwd=ROOT)
        host.run([exe],check=True,cwd=ROOT)

"""Native renderer checks and synthetic visual fixtures. No API calls."""
from pathlib import Path
import os, sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'/'host'))
from host_build import run
OUT=ROOT/'tests'/'wdgwars_map'/'render_previews'
OUT.mkdir(parents=True,exist_ok=True)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
exe=OUT/('render_test.exe' if os.name=='nt' else 'render_test')
run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
     '-I',ROOT/'applications_user/wdgwars_map',ROOT/'tests/wdgwars_map/render_test.c',
     ROOT/'applications_user/wdgwars_map/wdgwars_map_render.c',
     ROOT/'applications_user/wdgwars_map/wdgwars_map_world.c',
     ROOT/'applications_user/wdgwars_map/wdgwars_map_basemap.c',
     ROOT/'applications_user/wdgwars_map/wdgwars_map_json.c','-lm','-o',exe],check=True,cwd=ROOT)
run([exe,OUT],check=True,cwd=ROOT,timeout=60)
from PIL import Image
for source in OUT.glob('*.ppm'):
    image=Image.open(source).convert('RGB')
    assert image.size==(320,170)
    image.save(source.with_suffix('.png'))
    image.resize((640,340),Image.Resampling.NEAREST).save(source.with_name(source.stem+'-2x.png'))
print('Visual fixtures: synthetic territory scenes plus captured Greenwich OSM basemap:',OUT)
"""Strict native basemap tests. HTTP is mocked at the shared-client boundary."""
from pathlib import Path
import hashlib,json,sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tests/host'))
from host_build import run
app=root/'applications_user/wdgwars_map'
out=root/'build_wdgwars_map/host';out.mkdir(parents=True,exist_ok=True)
binary=out/'basemap'
run(['gcc','-std=c17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',
    '-I'+str(app),str(root/'tests/wdgwars_map/basemap_test.c'),
    str(app/'wdgwars_map_basemap.c'),str(app/'wdgwars_map_json.c'),'-lm','-o',str(binary)],check=True)
arguments=sys.argv[1:]
if arguments:
    run([str(binary)]+arguments,check=True)
else:
    for name,source_name in [('basemap_live_greenwich','basemap_live_source'),('basemap_live_manhattan','basemap_live_manhattan_source'),('basemap_live_center_first','basemap_live_center_first_source')]:
        fixture=root/f'tests/wdgwars_map/{name}.json'
        source=json.loads((root/f'tests/wdgwars_map/{source_name}.json').read_text())
        assert hashlib.sha256(fixture.read_bytes()).hexdigest()==source['sha256'], 'Captured-response provenance mismatch'
        run([str(binary),'--parse-file',str(fixture)]+[str(v) for v in source.get('bounds',[])],check=True)

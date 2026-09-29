from pathlib import Path
import importlib.util, os, tempfile, hashlib, json, argparse
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('host_build',ROOT/'tests/host/host_build.py')
host=importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
if os.name=='nt' and not os.environ.get('ZIG_CC'):
    os.environ['ZIG_CC']=str(ROOT/'.backups/bw16-evidence/native/ziglang/zig.exe')
parser=argparse.ArgumentParser(description="Replay production map clients with mocked transport; optional captures stay private.")
parser.add_argument('--private-overview',type=Path)
parser.add_argument('--private-local',type=Path)
args=parser.parse_args()
if args.private_local and not args.private_overview: parser.error('--private-local requires --private-overview')
fixture=ROOT/'tests/wdgwars_map/basemap_live_greenwich.json'
provenance=json.loads((ROOT/'tests/wdgwars_map/basemap_live_source.json').read_text())
assert hashlib.sha256(fixture.read_bytes()).hexdigest()==provenance['sha256']
with tempfile.TemporaryDirectory(prefix='wdg_integrated_http_') as d:
    exe=Path(d)/('integration.exe' if os.name=='nt' else 'integration')
    sources=['tests/wdgwars_map/client_integration_test.c','applications_user/wdgwars_map/wdgwars_map_basemap.c','applications_user/wdgwars_map/wdgwars_map_client.c','applications_user/wdgwars_map/wdgwars_map_json.c']
    host.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-O1','-g','-fsanitize=address,undefined','-I',ROOT/'tests/wdgwars_map/stubs','-I',ROOT/'applications_user/wdgwars_map',*[ROOT/s for s in sources],'-lm','-o',exe],check=True,cwd=ROOT)
    command=[exe,fixture]
    if args.private_overview: command.append(args.private_overview.resolve())
    if args.private_local: command.append(args.private_local.resolve())
    host.run(command,check=True,cwd=ROOT)

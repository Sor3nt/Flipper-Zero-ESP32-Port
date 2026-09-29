"""Run the bounded map tests and record release validation evidence."""
from pathlib import Path
import argparse,subprocess,sys,json,hashlib,datetime,os
R=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--firmware-build',type=Path,default=R/'build_t_embed_wdgwars_1_3')
group=parser.add_mutually_exclusive_group()
group.add_argument('--plus',action='store_true')
group.add_argument('--nonnrf-i2c',action='store_true')
args=parser.parse_args()
B=args.firmware_build.resolve()
if os.name=='nt' and not os.environ.get('ZIG_CC'): os.environ['ZIG_CC']=str(R/'.backups/bw16-evidence/native/ziglang/zig.exe')
checks=['run_parser.py','run_client.py','client_integration_run.py','basemap_run.py','cache_run.py','app_run.py','gps_run.py','render_run.py','check_abi.py']
results=[]
for name in checks:
 print('CHECK:',name,flush=True)
 command=[sys.executable,str(R/'tests/wdgwars_map'/name)]
 if name=='check_abi.py': command+=['--firmware-build',str(B),str(B/'fap/wdgwars_map.fap')]
 completed=subprocess.run(command,cwd=R,check=False,capture_output=True,text=True,encoding='utf-8',errors='replace')
 print(completed.stdout,flush=True)
 if completed.returncode: print(completed.stderr,flush=True); raise SystemExit(completed.returncode)
 results.append({'check':name,'exit_code':completed.returncode,'output':completed.stdout})
original_apps = [
 R/('build_t_embed_plus_i2c/fap/wardriver.fap' if args.plus else 'build_t_embed/fap/wardriver.fap'),
 R/('build_t_embed_plus_i2c/fap/bw16_r4tkn_i2c.fap' if (args.plus or args.nonnrf_i2c) else 'build_t_embed/fap/bw16_r4tkn.fap'),
]
for path in original_apps:
 name=path.name
 completed=subprocess.run([sys.executable,str(R/'tests/wdgwars_map/check_abi.py'),'--firmware-build',str(B),str(path)],cwd=R,check=False,capture_output=True,text=True,encoding='utf-8',errors='replace')
 print(completed.stdout,flush=True)
 if completed.returncode: print(completed.stderr,flush=True); raise SystemExit(completed.returncode)
 results.append({'check':'original FAP ABI: '+name,'exit_code':completed.returncode,'output':completed.stdout})
record={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'hardware_validated':False,'wdgwars_authenticated_validated':False,'checks':results,
'firmware_sha256':hashlib.sha256((B/'furi_esp32.bin').read_bytes()).hexdigest(),'fap_sha256':hashlib.sha256((B/'fap/wdgwars_map.fap').read_bytes()).hexdigest()}
(B/'VALIDATION.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
print('PASS complete standalone map validation; originals compatible')

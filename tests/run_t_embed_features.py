"""Run offline feature regressions; never access hardware, keys or live services.

Windows: set ZIG_CC to zig.exe. Linux: GCC with ASan/UBSan. Python needs Pillow
for renderer checks. Firmware/FAP linked-ABI checks run in the profile builder.
"""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECKS = [
    'host/run_wardriver_tests.py', 'host/run_wigle_tests.py',
    'host/run_radio_lifecycle_tests.py', 'host/run_rf_tests.py',
    'host/test_startup.py', 'host/test_upload_screen.py', 'host/test_shutdown.py',
    'bw16/run.py', 'bw16/run_legacy_guards.py', 'bw16_native/run.py', 'bw16_pcap/run.py',
    'bw16_capture_adapter/run.py', 'bw16_i2c_link/run.py',
    'bw16_i2c_hal/run.py', 'bw16_i2c_adapter/run.py', 'bw16_i2c_app/run.py',
    'wardriver_plus_i2c/run.py',
    'wdgwars_map/run_parser.py', 'wdgwars_map/run_client.py',
    'wdgwars_map/client_integration_run.py', 'wdgwars_map/basemap_run.py',
    'wdgwars_map/cache_run.py', 'wdgwars_map/app_run.py',
    'wdgwars_map/gps_run.py', 'wdgwars_map/render_run.py',
    'protopirate_rx/run.py',
    'plus_sd_recovery/hal_run.py', 'plus_sd_recovery/service_run.py',
    'plus_sd_recovery/startup_run.py', 'plus_sd_recovery/usb_handoff_run.py',
]


def main():
    out = ROOT / 'build_feature_tests'
    out.mkdir(exist_ok=True)
    (ROOT / '.backups').mkdir(exist_ok=True)
    (ROOT / 'build_t_embed/host_tests').mkdir(parents=True, exist_ok=True)
    results = []
    for name in CHECKS:
        print('CHECK', name, flush=True)
        log = out / (name.replace('/', '_') + '.log')
        with log.open('w', encoding='utf-8') as output:
            result = subprocess.run([sys.executable, ROOT / 'tests' / name],
                                    cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        results.append({'check': name, 'exit_code': result.returncode})
        if result.returncode:
            print(log.read_text(encoding='utf-8', errors='replace')[-10000:], flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    failed = [r['check'] for r in results if r['exit_code']]
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
    print('PASS:', len(results), 'offline suites. Hardware acceptance is separate.')


if __name__ == '__main__':
    main()

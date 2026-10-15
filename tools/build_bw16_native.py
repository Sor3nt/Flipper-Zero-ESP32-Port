"""Build only the isolated, source-built BW16 native bridge. Never open a COM port."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build_t_embed/bw16_native'
SDK = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) / 'Arduino15/packages/realtek'
CORE = SDK / 'hardware/AmebaD/3.1.9'
SUPPORT = SDK / 'tools/ameba_d_tools/1.1.3'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    if not CORE.is_dir() or not SUPPORT.is_dir():
        raise SystemExit('Requires installed Realtek AmebaD 3.1.9 and ameba_d_tools 1.1.3.')
    if sorted(p.name for p in CORE.parent.iterdir() if p.is_dir()) != ['3.1.9']:
        raise SystemExit('Ambiguous SDK versions: this helper requires 3.1.9 to be the only installed AmebaD core.')
    protected = [ROOT / p for p in (
        'build_t_embed/furi_esp32.bin',
        'build_t_embed/BW16-R4TKN-v4-TX0-Fix/km0_km4_image2.bin',
        'build_t_embed/fap/wardriver.fap',
    )]
    baseline = {str(p): sha(p) for p in protected if p.exists()}
    OUT.mkdir(parents=True, exist_ok=True)
    support_copy = OUT / 'tool_support'
    shutil.copytree(SUPPORT, support_copy, dirs_exist_ok=True)
    log_path = OUT / 'build.log'
    command = [shutil.which('arduino-cli') or 'arduino-cli', 'compile',
               '--fqbn', 'realtek:AmebaD:Ai-Thinker_BW16',
               '--build-path', str(OUT),
               '--build-property', 'runtime.tools.ameba_d_tools.path=' + str(support_copy),
               str(ROOT / 'firmware/bw16_controller')]
    started = time.time()
    with log_path.open('w', encoding='utf-8') as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print(log_path.read_text(encoding='utf-8', errors='replace')[-14000:])
        raise SystemExit(f'Native BW16 build failed: {log_path}')
    image = OUT / 'km0_km4_image2.bin'
    if not image.exists() or image.stat().st_mtime < started - 2:
        raise SystemExit('Build did not produce a fresh firmware image.')
    for name in ('km0_boot_all.bin', 'km4_boot_all.bin', 'imgtool_flashloader_amebad.bin'):
        shutil.copy2(support_copy / name, OUT / name)
    for path, digest in baseline.items():
        if sha(Path(path)) != digest:
            raise SystemExit('Protected artifact changed: ' + path)
    sources = sorted(p for p in (ROOT / 'firmware/bw16_controller').iterdir() if p.is_file())
    manifest = {
        'name': 'BW16 native controller 4.4 with passive and deauth-assisted PCAP capture',
        'sdk': 'realtek:AmebaD 3.1.9',
        'board': 'realtek:AmebaD:Ai-Thinker_BW16',
        'source_built': True,
        'upstream_r4tkn_release': False,
        'hardware_validated': False,
        'uart': 'RX1 PB2 commands; LOG_TX0 PA7 replies; 115200 8N1',
        'selected_visit_ms': 100, 'device_deadline_ms': 30000,
        'passive_capture_deadline_ms': 120000, 'capture_packet_limit': 256,
        'capture_deauth_max_attempts': 3, 'capture_deauth_window_ms': 1000,
        'capture_receiver_armed_before_deauth': True,
        'capture_linktype': 105, 'capture_uart_crc': 'CRC32 metadata and frame',
        'files': {p.name: {'sha256': sha(p), 'bytes': p.stat().st_size}
                  for p in sorted(OUT.glob('*.bin'))},
        'sources': {p.relative_to(ROOT).as_posix(): sha(p) for p in sources},
        'sdk_radio_library_sha256': sha(CORE / 'variants/common_libs/lib_wlan.a'),
        'protected_artifacts_unchanged': baseline,
    }
    (OUT / 'BUILD_INFO.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(image)
    print('SHA256:', sha(image), 'bytes:', image.stat().st_size)
    print('Build only. No UART or radio test performed.')

if __name__ == '__main__':
    main()

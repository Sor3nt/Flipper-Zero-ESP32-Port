"""Build only the isolated, source-built BW16 native bridge. Never open a COM port."""
from pathlib import Path
import sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
import hashlib
import json
import os
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build_t_embed_plus_i2c/bw16'
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
    support_copy = OUT.parent / 'bw16_tool_support'
    shutil.copytree(SUPPORT, support_copy, dirs_exist_ok=True)
    log_path = OUT.parent / 'bw16_build.log'
    command = [shutil.which('arduino-cli') or 'arduino-cli', 'compile',
               '--fqbn', 'realtek:AmebaD:Ai-Thinker_BW16',
               '--build-path', str(OUT),
               '--build-property', 'runtime.tools.ameba_d_tools.path=' + str(support_copy),
               '--build-property', 'compiler.c.extra_flags=-I' + (ROOT / 'firmware/bw16_controller_i2c').as_posix(),
               '--build-property', 'compiler.cpp.extra_flags=-I' + (ROOT / 'firmware/bw16_controller_i2c').as_posix(),
               str(ROOT / 'firmware/bw16_controller_i2c')]
    started = time.time()
    with log_path.open('w', encoding='utf-8') as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    OUT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(log_path, OUT / 'build.log')
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
    sources = sorted(p for d in ('firmware/bw16_controller','firmware/bw16_controller_i2c','firmware/bw16_i2c_link') for p in (ROOT / d).iterdir() if p.is_file())
    manifest = {
        'name': 'BW16 I2C controller 4.5 for CC1101 Plus',
        'sdk': 'realtek:AmebaD 3.1.9',
        'board': 'realtek:AmebaD:Ai-Thinker_BW16',
        'source_built': True,
        'upstream_r4tkn_release': False,
        'hardware_validated': False,
        'transport': 'I2C0 slave0x42; PA26 SDA; PA25 SCL; 100kHz; 64-byte CRC32 frames',
        'link_loss_stop_ms': 3000, 'i2c_tx_ring_bytes': 32768,
        'selected_visit_ms': 100, 'device_deadline_ms': 30000,
        'passive_capture_deadline_ms': 120000, 'capture_packet_limit': 256,
        'capture_deauth_max_attempts': 3, 'capture_deauth_window_ms': 1000,
        'capture_receiver_armed_before_deauth': True,
        'capture_linktype': 105, 'capture_stream_crc': 'CRC32 metadata and frame',
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

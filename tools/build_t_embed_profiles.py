"""Build a named T-Embed profile and its FAPs. Never flash or open a serial port.

Run in an activated ESP-IDF 5.4.1 environment. On Windows, Git Bash must be
available (BASH_EXE overrides discovery). Requires pyelftools and Pillow.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    'cc1101': ('Lilygo T-embed cc1101', 'sdkconfig.nonnrf_i2c_wdgwars', 'wardriver'),
    'cc1101-plus': ('Lily T-embed cc1101 plus', 'sdkconfig.plus_wdgwars', 'wardriver_plus_i2c'),
}


def verify_embedded_plugins(build, app):
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
    api = (ROOT/'components/flipper_application/flipper_application/firmware_api.c').read_text()
    exports = set(re.findall(r'/\*\s*(\w+)\s*\*/', api))
    info = subprocess.check_output(
        [sys.executable, ROOT/'tools/fap_app_info.py', 'applications_user/'+app],
        cwd=ROOT, text=True)
    entries = [line.split('\t') for line in info.splitlines()]
    expected = [row[1] for row in entries if row[0] == 'PLUGIN']
    for appid in expected:
        path = build/'fap/proto_pirate_assets/plugins'/(appid+'.fal')
        with path.open('rb') as stream:
            elf = ELFFile(stream)
            assert elf['e_machine'] == 'EM_XTENSA' and elf['e_type'] == 'ET_REL', path
            meta = struct.unpack_from('<IIHHHH', elf.get_section_by_name('.fapmeta').data())
            assert meta == (0x52474448, 1, 0, 1, 32, 0), (path, meta)
            imports = {symbol.name for symbol in elf.get_section_by_name('.symtab').iter_symbols()
                       if symbol['st_shndx'] == 'SHN_UNDEF' and symbol.name}
            assert imports <= exports, (path, imports - exports)
            for section in elf.iter_sections():
                if isinstance(section, RelocationSection) and elf.get_section(section['sh_info'])['sh_flags'] & 2:
                    assert all(rel['r_info_type'] in {0, 1, 11, 17, 18, 19, 20}
                               for rel in section.iter_relocations()), path
    print('Verified embedded protocol plugins:', len(expected))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=PROFILES, required=True)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--faps-only', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    try:
        import PIL
        import elftools
    except ImportError as error:
        parser.error(f'Missing Python dependency: {error.name}. Install Pillow and pyelftools in this environment.')
    label, config, wardriver = PROFILES[args.profile]
    build = ROOT / ('build_' + args.profile.replace('-', '_'))
    idf = Path(os.environ.get('IDF_PATH', '')) / 'tools/idf.py'
    if not idf.is_file():
        parser.error('Activate ESP-IDF 5.4.1 first (IDF_PATH and compiler PATH).')
    bash = os.environ.get('BASH_EXE') or shutil.which('bash')
    if os.name == 'nt' and not os.environ.get('BASH_EXE'):
        candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
        if candidate.exists():
            bash = str(candidate)
    if not bash:
        parser.error('Bash is required to build FAPs.')
    build.mkdir(exist_ok=True)
    config_copy = build / 'sdkconfig'
    env = dict(os.environ, FLIPPER_BOARD='lilygo_t_embed_cc1101',
               FAP_BOARD='lilygo_t_embed_cc1101', FAP_FW_BUILD_DIR=build.name,
               PYTHONUTF8='1')
    env.pop('MSYSTEM', None)
    env.pop('BASH_ENV', None)

    def run(command):
        subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)

    if not args.faps_only:
        shutil.copyfile(ROOT / config, config_copy)
        run([sys.executable, idf, '-B', build, '-D', 'SDKCONFIG=' + str(config_copy),
             '-D', 'FLIPPER_BOARD=lilygo_t_embed_cc1101', 'reconfigure'])
        run(['ninja', '-C', build, '-j', args.jobs])
    if not (build / 'furi_esp32.elf').is_file():
        parser.error('Build firmware before using --faps-only.')
    for app in ('bw16_r4tkn_i2c', wardriver, 'wdgwars_map', 'protopirate'):
        if app == 'wardriver_plus_i2c':
            env['FAP_OUTPUT_NAME'] = 'wardriver.fap'
        else:
            env.pop('FAP_OUTPUT_NAME', None)
        run([bash, 'buildFap.sh', 'applications_user/' + app])
        filename = {'wardriver_plus_i2c': 'wardriver', 'protopirate': 'proto_pirate'}.get(app, app)
        run([sys.executable, 'tests/wdgwars_map/check_abi.py', '--firmware-build', build,
             build / 'fap' / (filename + '.fap')])
        if app == 'protopirate':
            verify_embedded_plugins(build, app)
    artifacts = [p for p in build.rglob('*') if p.is_file() and
                 (p.suffix in ('.bin', '.fap', '.fal') or p.name == 'flash_args')]
    record = {'name': label, 'profile': args.profile, 'hardware_validated': False,
              'files': {p.relative_to(build).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in artifacts}}
    (build / 'PROFILE_BUILD.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Built and checked:', label, build)


if __name__ == '__main__':
    main()

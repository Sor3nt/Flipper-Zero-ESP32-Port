"""Check the produced FAP against the actual linked firmware, not just source."""
import argparse, pathlib, re, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

root=pathlib.Path(__file__).resolve().parents[2]
api=root/'components/flipper_application/flipper_application/firmware_api.c'
rows=re.findall(r'\.hash\s*=\s*(0x[0-9a-fA-F]+).*?/\*\s*(\S+)\s*\*/',api.read_text())
def djb2(name):
    h=0x1505
    for c in name.encode(): h=(h*33+c)&0xffffffff
    return h
hashes=[int(h,16) for h,n in rows]
assert hashes==sorted(hashes) and len(hashes)==len(set(hashes)), 'API sort/collision'
assert all(int(h,16)==djb2(n) for h,n in rows), 'API hash mismatch'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('fap',nargs='?',type=pathlib.Path,help='FAP to inspect; defaults to the selected firmware build/fap/wdgwars_map.fap')
parser.add_argument('--firmware-build',type=pathlib.Path,default=root/'build_t_embed_wdgwars',help='Actual linked firmware build directory (ELF and build.ninja)')
parser.add_argument('--expected-stack',type=int,default=16384,help='Expected app stack after the ESP32 builder minimum is applied')
args=parser.parse_args()
build=args.firmware_build.resolve()
wrapped=set(re.findall(r'--wrap[=,](\w+)',(build/'build.ninja').read_text()))
app_path=args.fap.resolve() if args.fap else build/'fap/wdgwars_map.fap'
with (build/'furi_esp32.elf').open('rb') as f, app_path.open('rb') as a:
    fw,app=ELFFile(f),ELFFile(a)
    assert fw['e_machine']==app['e_machine']=='EM_XTENSA'
    assert app['e_type']=='ET_REL' and app.elfclass==32 and app.little_endian
    sym={s.name:s for s in fw.get_section_by_name('.symtab').iter_symbols()}
    table=sym['firmware_api_table']
    section=fw.get_section(table['st_shndx'])
    offset=table['st_value']-section['sh_addr']
    raw=section.data()[offset:offset+table['st_size']]
    linked=dict(struct.iter_unpack('<II',raw))
    imports=[s.name for s in app.get_section_by_name('.symtab').iter_symbols()
             if s['st_shndx']=='SHN_UNDEF' and s.name]
    missing=sorted(n for n in imports if not linked.get(djb2(n)))
    assert not missing, f'Missing imports in linked firmware {build}: {missing}'
    assert len(linked)==len(rows), f'Linked firmware has {len(linked)} API entries; current source has {len(rows)}'
    for h,n in rows:
        # IDF intentionally redirects e.g. longjmp to __wrap_longjmp. Check the
        # actual linker's --wrap options, not just the unwrapped ROM symbol.
        resolved='__wrap_'+n if n in wrapped else n
        assert linked[int(h,16)]==sym[resolved]['st_value'], f'Incorrect linked export: {n}'
    exports={n for h,n in rows}
    assert set(imports)<=exports, f'Missing imports: {set(imports)-exports}'
    for n in imports: assert djb2(n) in linked and linked[djb2(n)]!=0
    meta=app.get_section_by_name('.fapmeta').data()
    magic,version,minor,major,target,stack=struct.unpack_from('<IIHHHH',meta)
    assert (magic,version,major,minor,target)==(0x52474448,1,1,0,32)
    assert stack==args.expected_stack, f'App stack {stack} does not match expected {args.expected_stack}'
    assert len(meta)==85
    if app_path.name=='wdgwars_map.fap':
        assert meta[52]==1, 'Missing WD app icon'
        from PIL import Image
        icon=Image.open(root/'applications_user/wdgwars_map/wdgwars_icon.pbm').convert('1')
        assert icon.size==(10,10)
        assert meta[53]==0, 'Expected uncompressed icon'
        for y in range(10):
            for x in range(10):
                pixel=(meta[54+y*2+x//8]>>(x%8))&1
                assert pixel==(1 if icon.getpixel((x,y))==0 else 0), 'App icon truncated/mismatched'

    interface=sym['firmware_api_impl']
    section=fw.get_section(interface['st_shndx'])
    offset=interface['st_value']-section['sh_addr']
    assert struct.unpack_from('<HH',section.data(),offset)==(major,minor)
    assert app.get_section_by_name('.symtab') and app.get_section_by_name('.rela.text')
    relocation_types=set()
    for section in app.iter_sections():
        if isinstance(section,RelocationSection):
            target_section=app.get_section(section['sh_info'])
            if target_section['sh_flags'] & 2: # Only runtime-allocated sections.
                relocation_types.update(r['r_info_type'] for r in section.iter_relocations())
    assert relocation_types <= {0,1,11,17,18,19,20}, f'Unsupported relocations: {relocation_types}'
    print(f"Firmware: {build / 'furi_esp32.elf'}")
    print(f'App: {app_path}')
    print(f'PASS: Xtensa ELF32 relocatable, API {major}.{minor}, target {target}, stack {stack}')
    print(f'PASS: {len(rows)} linked API entries sorted, unique, correct hash and address')
    print(f'PASS: all {len(imports)} FAP imports resolve in linked firmware')
    print(f'PASS: allocated-section relocation types supported by loader: {sorted(relocation_types)}')
    if app_path.name == 'bw16_r4tkn_i2c.fap':
        assert not any(n.startswith('uart_') for n in imports), 'Unexpected UART dependency'
    print('UART imports: '+', '.join(n for n in imports if n.startswith('uart_')))

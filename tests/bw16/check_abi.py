"""Check the produced FAP against the actual linked firmware, not just source."""
import pathlib, re, struct, sys
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
build=root/'build_t_embed'
wrapped=set(re.findall(r'--wrap[=,](\w+)',(build/'build.ninja').read_text()))
app_path=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else build/'fap/bw16_r4tkn.fap'
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
    assert len(linked)==len(rows)
    for h,n in rows:
        # IDF intentionally redirects e.g. longjmp to __wrap_longjmp. Check the
        # actual linker's --wrap options, not just the unwrapped ROM symbol.
        resolved='__wrap_'+n if n in wrapped else n
        assert linked[int(h,16)]==sym[resolved]['st_value'], f'Incorrect linked export: {n}'
    imports=[s.name for s in app.get_section_by_name('.symtab').iter_symbols()
             if s['st_shndx']=='SHN_UNDEF' and s.name]
    exports={n for h,n in rows}
    assert set(imports)<=exports, f'Missing imports: {set(imports)-exports}'
    for n in imports: assert djb2(n) in linked and linked[djb2(n)]!=0
    meta=app.get_section_by_name('.fapmeta').data()
    magic,version,minor,major,target,stack=struct.unpack_from('<IIHHHH',meta)
    assert (magic,version,major,minor,target,stack)==(0x52474448,1,1,0,32,16384)
    assert len(meta)==85
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
    print(f'App: {app_path}')
    print(f'PASS: Xtensa ELF32 relocatable, API {major}.{minor}, target {target}, stack {stack}')
    print(f'PASS: {len(rows)} linked API entries sorted, unique, correct hash and address')
    print(f'PASS: all {len(imports)} FAP imports resolve in linked firmware')
    print(f'PASS: allocated-section relocation types supported by loader: {sorted(relocation_types)}')
    print('UART imports: '+', '.join(n for n in imports if n.startswith('uart_')))

"""Check the preserved FAP against the ACTUAL linked firmware export table.

Requires pyelftools. Read-only: does not rebuild or replace the working SD FAP.
"""
from pathlib import Path
from elftools.elf.elffile import ELFFile
import struct
import sys

root = Path(__file__).resolve().parents[2]

def gnu_hash(name):
    value = 0x1505
    for byte in name.encode():
        value = (value * 33 + byte) & 0xFFFFFFFF
    return value

fap = Path(sys.argv[1]) if len(sys.argv)>1 else root / "sdcard/apps/Tools/wardriver.fap"
with fap.open("rb") as package:
    symbols = ELFFile(package).get_section_by_name(".symtab")
    imports = [symbol.name for symbol in symbols.iter_symbols()
               if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"]

with (root / "build_t_embed/furi_esp32.elf").open("rb") as firmware:
    elf = ELFFile(firmware)
    table = elf.get_section_by_name(".symtab").get_symbol_by_name("firmware_api_table")
    assert table and len(table) == 1, "Firmware API table not present in linked ELF"
    table = table[0]
    section = elf.get_section(table["st_shndx"])
    offset = table["st_value"] - section["sh_addr"]
    data = section.data()[offset:offset + table["st_size"]]
    assert data and len(data) % 8 == 0
    entries = list(struct.iter_unpack("<II", data))
    assert all(a[0] < b[0] for a, b in zip(entries, entries[1:])), "Unsorted export hashes"
    exports = dict(entries)
    unresolved = [name for name in imports if not exports.get(gnu_hash(name))]
    assert not unresolved, "Missing linked Wardriver exports: " + repr(unresolved)
    print(f"PASS: all {len(imports)} Wardriver FAP imports resolved in linked firmware ELF: {fap.name}")

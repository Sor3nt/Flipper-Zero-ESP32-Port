#!/usr/bin/env python3
"""
Check if all undefined symbols in a FAP are available in the firmware API table.
Usage: check_fap_symbols.py <firmware_api.c> <symbol_list_file>
"""
import sys
import re


def elf_gnu_hash(s: str) -> int:
    h = 0x1505
    for c in s.encode():
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def load_api_hashes(api_file: str) -> set[int]:
    """Extract all hash values from firmware_api.c"""
    hashes = set()
    with open(api_file) as f:
        for line in f:
            m = re.search(r'\.hash\s*=\s*(0x[0-9a-fA-F]+)', line)
            if m:
                hashes.add(int(m.group(1), 16))
    return hashes


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <firmware_api.c> <symbols_file>")
        sys.exit(1)

    api_file = sys.argv[1]
    symbols_file = sys.argv[2]

    api_hashes = load_api_hashes(api_file)

    with open(symbols_file) as f:
        symbols = [line.strip() for line in f if line.strip()]

    missing = []
    for sym in symbols:
        h = elf_gnu_hash(sym)
        if h not in api_hashes:
            missing.append((sym, h))

    if missing:
        print(f"  ⚠ {len(missing)} missing API symbols:")
        for sym, h in sorted(missing):
            print(f"    {sym} (hash=0x{h:08x})")
        return 1
    else:
        print(f"  ✓ All {len(symbols)} symbols resolved")
        return 0


if __name__ == "__main__":
    sys.exit(main())

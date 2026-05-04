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
    """Extract all hash values from firmware_api.c and verify sort order."""
    hashes = set()
    prev_hash = -1
    prev_line_no = 0
    sort_errors = []
    with open(api_file) as f:
        for line_no, line in enumerate(f, 1):
            m = re.search(r'\.hash\s*=\s*(0x[0-9a-fA-F]+)', line)
            if m:
                h = int(m.group(1), 16)
                if h <= prev_hash:
                    sort_errors.append(
                        f"  line {line_no}: 0x{h:08x} <= previous 0x{prev_hash:08x} (line {prev_line_no})"
                    )
                prev_hash = h
                prev_line_no = line_no
                hashes.add(h)
    if sort_errors:
        print(f"  ⚠ API table sort errors (binary search will fail at runtime!):")
        for err in sort_errors:
            print(err)
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
        print(f"  [!] {len(missing)} missing API symbols:")
        for sym, h in sorted(missing):
            print(f"    {sym} (hash=0x{h:08x})")
        return 1
    else:
        print(f"  [OK] All {len(symbols)} symbols resolved")
        return 0


if __name__ == "__main__":
    sys.exit(main())

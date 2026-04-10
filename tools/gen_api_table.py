#!/usr/bin/env python3
"""
Generate sorted sym_entry table for firmware API.
Usage: python3 gen_api_table.py func1 func2 func3 ...
"""
import sys


def elf_gnu_hash(s: str) -> int:
    h = 0x1505
    for c in s.encode():
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 gen_api_table.py func1 func2 ...")
        print("       python3 gen_api_table.py -f <file_with_names>")
        sys.exit(1)

    names = []
    if sys.argv[1] == "-f":
        with open(sys.argv[2]) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    names.append(line)
    else:
        names = sys.argv[1:]

    entries = []
    for name in names:
        h = elf_gnu_hash(name)
        entries.append((h, name))

    # Check for collisions
    hashes = [e[0] for e in entries]
    if len(hashes) != len(set(hashes)):
        print("WARNING: Hash collision detected!", file=sys.stderr)
        seen = {}
        for h, name in entries:
            if h in seen:
                print(f"  Collision: {name} and {seen[h]} both hash to 0x{h:08x}", file=sys.stderr)
            seen[h] = name

    # Sort by hash
    entries.sort(key=lambda e: e[0])

    print("static const struct sym_entry firmware_api_table[] = {")
    for h, name in entries:
        print(f"    {{ .hash = 0x{h:08x}, .address = (uint32_t){name} }}, /* {name} */")
    print("};")


if __name__ == "__main__":
    main()

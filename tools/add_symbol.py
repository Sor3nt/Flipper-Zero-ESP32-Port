#!/usr/bin/env python3
"""
Add symbols to the firmware API table (firmware_api.c) at the correct sorted position.

Usage:
    python3 tools/add_symbol.py symbol1 symbol2 ...
    python3 tools/add_symbol.py -f symbols.txt
    python3 tools/add_symbol.py --dry-run symbol1 symbol2

Automatically determines the correct .address field:
  - I_* icons:             (uint32_t)&I_name
  - message_* messages:    (uint32_t)&message_name
  - sequence_* sequences:  (uint32_t)&sequence_name
  - everything else:       (uint32_t)function_name
"""
import sys
import os
import re
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
API_FILE = os.path.join(
    PROJECT_DIR,
    "components", "flipper_application", "flipper_application", "firmware_api.c",
)


def elf_gnu_hash(s: str) -> int:
    h = 0x1505
    for c in s.encode():
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def needs_address_of(name: str) -> bool:
    """Return True if the symbol is a global variable (needs & prefix)."""
    return name.startswith("I_") or name.startswith("message_") or name.startswith("sequence_")


def format_entry(name: str, h: int) -> str:
    prefix = "&" if needs_address_of(name) else ""
    return f"    {{ .hash = 0x{h:08x}, .address = (uint32_t){prefix}{name} }}, /* {name} */"


def parse_table(lines: list[str]) -> tuple[int, int, list[tuple[int, str, str]]]:
    """Parse firmware_api.c and return (table_start, table_end, entries).

    Each entry is (hash, name, original_line).
    table_start/table_end are line indices (0-based) of the first/last entry lines.
    """
    entries = []
    table_start = None
    table_end = None

    for i, line in enumerate(lines):
        m = re.search(r'\.hash\s*=\s*(0x[0-9a-fA-F]+).*?/\*\s*(\S+)\s*\*/', line)
        if m:
            if table_start is None:
                table_start = i
            table_end = i
            h = int(m.group(1), 16)
            name = m.group(2)
            entries.append((h, name, line))

    return table_start, table_end, entries


def main():
    parser = argparse.ArgumentParser(description="Add symbols to firmware API table")
    parser.add_argument("symbols", nargs="*", help="Symbol names to add")
    parser.add_argument("-f", "--file", help="File with symbol names (one per line)")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be done without modifying files")
    args = parser.parse_args()

    # Collect symbol names
    names = list(args.symbols)
    if args.file:
        with open(args.file) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    names.append(line)

    if not names:
        parser.print_help()
        sys.exit(1)

    # Read current file
    with open(API_FILE) as f:
        lines = f.readlines()

    table_start, table_end, entries = parse_table(lines)
    if table_start is None:
        print("ERROR: Could not find API table in firmware_api.c", file=sys.stderr)
        sys.exit(1)

    existing_hashes = {h for h, _, _ in entries}
    existing_names = {name for _, name, _ in entries}

    # Process new symbols
    to_add = []
    skipped = []
    for name in names:
        if name in existing_names:
            skipped.append((name, "already exists"))
            continue
        h = elf_gnu_hash(name)
        if h in existing_hashes:
            collision = next(n for hh, n, _ in entries if hh == h)
            skipped.append((name, f"hash collision with '{collision}' (0x{h:08x})"))
            continue
        to_add.append((h, name))

    if skipped:
        for name, reason in skipped:
            print(f"  skip: {name} ({reason})")

    if not to_add:
        print("Nothing to add.")
        return 0

    if args.dry_run:
        print(f"Would add {len(to_add)} symbol(s) to {API_FILE}:")
        for h, name in sorted(to_add, key=lambda x: x[0]):
            prefix = "&" if needs_address_of(name) else ""
            print(f"  + {name} (0x{h:08x}) -> {prefix}{name}")
        return 0

    # Merge existing + new entries, sort, and rebuild the table section.
    # WICHTIG: bestehende Zeilen bleiben verbatim erhalten — nur neue Einträge
    # werden via format_entry() formatiert. Sonst gehen manuell gesetzte
    # &-Prefixe für Variablen außerhalb der I_*/message_*/sequence_*-Convention
    # verloren (z.B. ble_profile_serial, subghz_protocol_registry).
    merged = [(h, name, original_line) for h, name, original_line in entries]
    for h, name in to_add:
        merged.append((h, name, None))
    merged.sort(key=lambda x: x[0])

    new_table_lines = [
        (original_line if original_line is not None else format_entry(name, h) + "\n")
        for h, name, original_line in merged
    ]

    # Replace table lines in the file
    result = lines[:table_start] + new_table_lines + lines[table_end + 1:]

    with open(API_FILE, "w") as f:
        f.writelines(result)

    # Verify sort order of result
    with open(API_FILE) as f:
        verify_lines = f.readlines()
    _, _, verify_entries = parse_table(verify_lines)
    sort_ok = all(verify_entries[i][0] < verify_entries[i + 1][0] for i in range(len(verify_entries) - 1))

    print(f"  Added {len(to_add)} symbol(s) to firmware_api.c ({len(verify_entries)} total)")
    for h, name in sorted(to_add, key=lambda x: x[0]):
        prefix = "&" if needs_address_of(name) else ""
        print(f"    + {name} (0x{h:08x})")

    if not sort_ok:
        print("  WARNING: Table sort order is broken!", file=sys.stderr)
        return 1

    print(f"  ✓ Table correctly sorted ({len(verify_entries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

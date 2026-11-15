#!/usr/bin/env python3
"""Erzeugt sdcard/files.txt — ein Manifest für den "Update SD"-Delta-Sync.

Läuft über den lokalen sdcard/-Ordner und schreibt pro Datei eine Zeile:

    <sha256hex> <size> <relpfad>

Das Gerät lädt dieses Manifest (bei Versions-Mismatch), vergleicht jede
Datei per SHA256 mit der lokalen Kopie auf der SD-Karte und lädt nur
neue/geänderte Dateien einzeln nach (statt der kompletten sdcard.zip).

Der sdcard/-Ordner wird gespiegelt unter
https://sor3nt.github.io/release/t-embed/latest/ veröffentlicht, d.h. eine
Datei `apps/foo.fap` ist erreichbar unter
https://sor3nt.github.io/release/t-embed/latest/apps/foo.fap
"""

import hashlib
import os
import sys
from pathlib import Path

SDCARD_DIR = Path(__file__).resolve().parent / "sdcard"
MANIFEST_NAME = "files.txt"

# Diese Einträge gehören nicht auf die SD-Karte / sind Build-Artefakte.
EXCLUDE_NAMES = {".DS_Store", MANIFEST_NAME, "Thumbs.db"}


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if not SDCARD_DIR.is_dir():
        print(f"error: {SDCARD_DIR} not found", file=sys.stderr)
        return 1

    entries = []
    for root, dirs, files in os.walk(SDCARD_DIR):
        dirs.sort()
        for name in sorted(files):
            if name in EXCLUDE_NAMES:
                continue
            full = Path(root) / name
            if not full.is_file() or full.is_symlink():
                continue
            rel = full.relative_to(SDCARD_DIR).as_posix()
            entries.append((rel, full.stat().st_size, sha256_of(full)))

    entries.sort(key=lambda e: e[0])

    manifest = SDCARD_DIR / MANIFEST_NAME
    with manifest.open("w", encoding="utf-8", newline="\n") as out:
        for rel, size, digest in entries:
            out.write(f"{digest} {size} {rel}\n")

    print(f"{manifest}: {len(entries)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

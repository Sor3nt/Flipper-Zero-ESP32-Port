#!/usr/bin/env python3
"""Copy a directory tree into the FAM runtime layout, omitting bulky SD-only files.

Flipper infrared libraries (*.ir) belong on removable storage (EXT_PATH), not in the
embedded firmware image, so smaller boards keep headroom as the app set grows."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def copy_tree_omit_suffixes(src: Path, dst: Path, omit_suffixes: frozenset[str]) -> None:
    if not src.exists():
        raise SystemExit(f"copy_ext_tree: missing source {src}")
    if src.is_file():
        if src.suffix.lower() in omit_suffixes:
            return
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        return
    if not src.is_dir():
        return
    dst.mkdir(parents=True, exist_ok=True)
    for path in src.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() in omit_suffixes:
            continue
        rel = path.relative_to(src)
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("src", type=Path, help="Source file or directory")
    parser.add_argument("dst", type=Path, help="Destination file or directory")
    parser.add_argument(
        "--omit-suffix",
        action="append",
        default=[],
        metavar="EXT",
        help="File suffix to skip (repeatable), e.g. .ir",
    )
    args = parser.parse_args()
    raw = args.omit_suffix or [".ir"]
    norm: list[str] = []
    for s in raw:
        x = s.strip().lower()
        if not x:
            continue
        norm.append(x if x.startswith(".") else f".{x}")
    copy_tree_omit_suffixes(args.src.resolve(), args.dst, frozenset(norm))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

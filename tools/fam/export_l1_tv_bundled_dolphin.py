#!/usr/bin/env python3
"""Extract L1_Tv frames from assets_dolphin_internal.c into assets/ext_defaults/dolphin."""

from __future__ import annotations

import re
import sys
from pathlib import Path

BUNDLED_NAME = "L1_FwBundled_128x47"
META_TXT = """Filetype: Flipper Animation
Version: 1

Width: 128
Height: 47
Passive frames: 6
Active frames: 2
Frames order: 0 1 2 3 4 5 6 7
Active cycles: 2
Frame rate: 2
Duration: 3600
Active cooldown: 5

Bubble slots: 2

Slot: 0
X: 1
Y: 23
Text: Take the red pill
AlignH: Right
AlignV: Bottom
StartFrame: 7
EndFrame: 9

Slot: 1
X: 1
Y: 23
Text: I can joke better
AlignH: Right
AlignV: Bottom
StartFrame: 7
EndFrame: 9
"""

MANIFEST_TXT = f"""Filetype: Flipper Animation Manifest
Version: 1

Name: {BUNDLED_NAME}
Min butthurt: 0
Max butthurt: 14
Min level: 1
Max level: 3
Weight: 3
"""


def parse_c_uint8_array(body: str) -> bytes:
    parts = re.split(r"[,\s]+", body.strip().strip(","))
    out = bytearray()
    for p in parts:
        if not p:
            continue
        out.append(int(p, 0) & 0xFF)
    return bytes(out)


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    c_path = root / "components" / "desktop" / "animations" / "assets_dolphin_internal.c"
    out_dir = root / "assets" / "ext_defaults" / "dolphin" / BUNDLED_NAME
    text = c_path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"const uint8_t (_A_L1_Tv_128x47_(\d+))\[\] = \{([^}]+)\};", re.MULTILINE | re.DOTALL
    )
    matches = list(pattern.finditer(text))
    if not matches:
        print("No L1_Tv arrays found", file=sys.stderr)
        return 1
    out_dir.mkdir(parents=True, exist_ok=True)
    for m in matches:
        idx = int(m.group(2))
        raw = parse_c_uint8_array(m.group(3))
        (out_dir / f"frame_{idx}.bm").write_bytes(raw)
    (out_dir / "meta.txt").write_text(META_TXT, encoding="utf-8", newline="\n")
    manifest_path = root / "assets" / "ext_defaults" / "dolphin" / "manifest.txt"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(MANIFEST_TXT, encoding="utf-8", newline="\n")
    (root / "assets" / "ext_defaults" / "asset_packs").mkdir(parents=True, exist_ok=True)
    print(f"Wrote {len(matches)} frames to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

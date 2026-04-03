#!/usr/bin/env python3

from __future__ import annotations

import argparse
import io
import os
import re
from pathlib import Path

from PIL import Image, ImageOps


ICONS_TEMPLATE_H_HEADER = """#pragma once

#include <gui/icon.h>
#include <assets_icons.h>

"""
ICONS_TEMPLATE_H_ICON_NAME = "extern const Icon {name};\n"

ICONS_TEMPLATE_C_HEADER = """#include "{assets_filename}.h"

#include <gui/icon_i.h>

"""
ICONS_TEMPLATE_C_FRAME = "const uint8_t {name}[] = {data};\n"
ICONS_TEMPLATE_C_DATA = "const uint8_t* const {name}[] = {data};\n"
ICONS_TEMPLATE_C_ICONS = (
    "const Icon {name} = "
    "{{.width={width},.height={height},.frame_count={frame_count},.frame_rate={frame_rate},.frames=_{name}}};\n"
)


def png_to_icon_payload(path: Path) -> tuple[int, int, str]:
    with Image.open(path) as image:
        with io.BytesIO() as output:
            bw = ImageOps.invert(image.convert("1"))
            bw.save(output, format="XBM")
            xbm = output.getvalue().decode().strip()

    lines = xbm.splitlines()
    width = int(lines[0].strip().split(" ")[2])
    height = int(lines[1].strip().split(" ")[2])
    data = "".join(lines[2:]).replace(" ", "").split("=")[1][1:-2]
    data_hex = data.replace(",", " ").replace("0x", "")
    raw = bytearray.fromhex(data_hex)
    encoded = bytearray([0x00])
    encoded.extend(raw)
    c_array = "{" + "".join(f"0x{byte:02x}," for byte in encoded) + "}"
    return width, height, c_array


def load_global_icon_symbols() -> set[str]:
    assets_header = Path(__file__).resolve().parents[2] / "components/assets/assets_icons.h"
    if not assets_header.exists():
        return set()

    symbols: set[str] = set()
    for line in assets_header.read_text(encoding="utf-8").splitlines():
        match = re.match(r"extern const Icon ([A-Za-z0-9_]+);", line.strip())
        if match:
            symbols.add(match.group(1))
    return symbols


def generate_icons(input_directory: Path, output_directory: Path, filename: str) -> int:
    output_directory.mkdir(parents=True, exist_ok=True)

    icons: list[tuple[str, int, int, int, int]] = []
    global_icons = load_global_icon_symbols()
    c_path = output_directory / f"{filename}.c"
    h_path = output_directory / f"{filename}.h"

    with c_path.open("w", encoding="utf-8", newline="\n") as icons_c:
        icons_c.write(ICONS_TEMPLATE_C_HEADER.format(assets_filename=filename))

        for dirpath, dirnames, filenames in os.walk(input_directory):
            dirnames.sort()
            filenames.sort()
            if not filenames:
                continue

            current_dir = Path(dirpath)

            if "frame_rate" in filenames:
                icon_name = "A_" + current_dir.name.replace("-", "_")
                if icon_name in global_icons:
                    continue
                frame_rate = int((current_dir / "frame_rate").read_text(encoding="utf-8").strip())
                width = None
                height = None
                frame_names: list[str] = []

                for frame_index, frame_name in enumerate(name for name in filenames if name.endswith(".png")):
                    frame_path = current_dir / frame_name
                    frame_width, frame_height, payload = png_to_icon_payload(frame_path)
                    if width is None:
                        width = frame_width
                    if height is None:
                        height = frame_height
                    if width != frame_width or height != frame_height:
                        raise ValueError(f"Animation frames differ in size for {current_dir}")
                    symbol_name = f"_{icon_name}_{frame_index}"
                    frame_names.append(symbol_name)
                    icons_c.write(ICONS_TEMPLATE_C_FRAME.format(name=symbol_name, data=payload))

                if not frame_names or width is None or height is None:
                    raise ValueError(f"Animation folder {current_dir} does not contain PNG frames")

                icons_c.write(
                    ICONS_TEMPLATE_C_DATA.format(
                        name=f"_{icon_name}",
                        data="{" + ",".join(frame_names) + "}",
                    )
                )
                icons_c.write("\n")
                icons.append((icon_name, width, height, frame_rate, len(frame_names)))
                continue

            for file_name in filenames:
                if not file_name.endswith(".png"):
                    continue

                icon_name = "I_" + "_".join(file_name.split(".")[:-1]).replace("-", "_")
                if icon_name in global_icons:
                    continue
                width, height, payload = png_to_icon_payload(current_dir / file_name)
                frame_name = f"_{icon_name}_0"
                icons_c.write(ICONS_TEMPLATE_C_FRAME.format(name=frame_name, data=payload))
                icons_c.write(
                    ICONS_TEMPLATE_C_DATA.format(name=f"_{icon_name}", data="{" + frame_name + "}")
                )
                icons_c.write("\n")
                icons.append((icon_name, width, height, 0, 1))

        for name, width, height, frame_rate, frame_count in icons:
            icons_c.write(
                ICONS_TEMPLATE_C_ICONS.format(
                    name=name,
                    width=width,
                    height=height,
                    frame_rate=frame_rate,
                    frame_count=frame_count,
                )
            )
        icons_c.write("\n")

    with h_path.open("w", encoding="utf-8", newline="\n") as icons_h:
        icons_h.write(ICONS_TEMPLATE_H_HEADER)
        for name, _, _, _, _ in icons:
            icons_h.write(ICONS_TEMPLATE_H_ICON_NAME.format(name=name))

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile Flipper-style icon bundles")
    subparsers = parser.add_subparsers(dest="command", required=True)

    icons = subparsers.add_parser("icons", help="Compile icon bundle")
    icons.add_argument("input_directory")
    icons.add_argument("output_directory")
    icons.add_argument("--filename", default="assets_icons")

    args = parser.parse_args()
    if args.command != "icons":
        raise ValueError(f"Unsupported command: {args.command}")

    return generate_icons(
        Path(args.input_directory),
        Path(args.output_directory),
        args.filename,
    )


if __name__ == "__main__":
    raise SystemExit(main())

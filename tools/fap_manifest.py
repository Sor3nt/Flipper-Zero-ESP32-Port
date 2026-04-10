#!/usr/bin/env python3
"""
Generate a binary .fapmeta section for a Flipper Application Package.

The manifest format matches FlipperApplicationManifestV1:
  - manifest_magic:      uint32  (0x52474448)
  - manifest_version:    uint32  (1)
  - api_version_minor:   uint16
  - api_version_major:   uint16
  - hardware_target_id:  uint16
  - stack_size:          uint16
  - app_version:         uint32
  - name:                char[32]
  - has_icon:            char
  - icon:                char[32]
"""
import argparse
import struct


def main():
    parser = argparse.ArgumentParser(description="Generate FAP manifest binary")
    parser.add_argument("--name", required=True, help="Application name (max 32 chars)")
    parser.add_argument("--api-major", type=int, default=1, help="API major version")
    parser.add_argument("--api-minor", type=int, default=0, help="API minor version")
    parser.add_argument("--target", type=int, default=32, help="Hardware target ID")
    parser.add_argument("--stack-size", type=int, default=4096, help="Stack size in bytes")
    parser.add_argument("--app-version", type=int, default=1, help="Application version")
    parser.add_argument("--output", required=True, help="Output binary file")
    args = parser.parse_args()

    FAP_MANIFEST_MAGIC = 0x52474448
    FAP_MANIFEST_VERSION = 1

    # Pack the name (32 bytes, null-padded)
    name_bytes = args.name.encode("utf-8")[:32]
    name_bytes = name_bytes + b"\x00" * (32 - len(name_bytes))

    # No icon
    has_icon = 0
    icon_bytes = b"\x00" * 32

    # struct FlipperApplicationManifestV1 (packed)
    data = struct.pack(
        "<"       # little-endian
        "I"       # manifest_magic (uint32)
        "I"       # manifest_version (uint32)
        "HH"      # api_version: minor (uint16), major (uint16)
        "H"       # hardware_target_id (uint16)
        "H"       # stack_size (uint16)
        "I"       # app_version (uint32)
        "32s"     # name (char[32])
        "c"       # has_icon (char)
        "32s",    # icon (char[32])
        FAP_MANIFEST_MAGIC,
        FAP_MANIFEST_VERSION,
        args.api_minor,
        args.api_major,
        args.target,
        args.stack_size,
        args.app_version,
        name_bytes,
        bytes([has_icon]),
        icon_bytes,
    )

    with open(args.output, "wb") as f:
        f.write(data)

    print(f"Manifest: {args.name}, API {args.api_major}.{args.api_minor}, "
          f"target {args.target}, stack {args.stack_size}, "
          f"size {len(data)} bytes")


if __name__ == "__main__":
    main()

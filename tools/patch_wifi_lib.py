#!/usr/bin/env python3
"""
Patch ESP-IDF's libnet80211.a to allow raw TX of deauth/disassoc frames.

The closed-source WiFi blob's ieee80211_raw_frame_sanity_check() rejects
management frame subtypes 0xA0 (disassoc) and 0xC0 (deauth). This script
patches the function body in ieee80211_output.o to return 0 (ESP_OK)
immediately, then re-archives it.

Usage: patch_wifi_lib.py <libnet80211.a> <objcopy> <ar> <output.a>
"""
import sys, subprocess, shutil, tempfile, os, struct

FUNC_SECTION = ".text.ieee80211_raw_frame_sanity_check"

# Xtensa patch: entry a1,64 ; movi.n a2,0 ; retw.n
# Original: 36 81 00 ...
# Patched:  36 81 00 0c 02 1d f0
PATCH_ENTRY = bytes([0x36, 0x81, 0x00])  # entry a1, 64 (keep)
PATCH_BODY  = bytes([0x0c, 0x02, 0x1d, 0xf0])  # movi.n a2, 0; retw.n


def find_section_offset(objdump, obj_path, section_name):
    """Find file offset and size of a section in an ELF .o file."""
    out = subprocess.check_output(
        [objdump, "-h", obj_path], text=True
    )
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[1] == section_name:
            size = int(parts[2], 16)
            file_off = int(parts[5], 16)
            return file_off, size
    return None, None


def find_func_offset_in_section(objdump, obj_path, section_name):
    """Find the function entry offset within its section (after literal pool)."""
    out = subprocess.check_output(
        [objdump, "-d", "-j", section_name, obj_path], text=True
    )
    for line in out.splitlines():
        if "entry" in line and ":" in line:
            # Format: "  5c:  008136    entry a1, 64"
            addr_str = line.strip().split(":")[0].strip()
            return int(addr_str, 16)
    return None


def main():
    if len(sys.argv) != 5:
        print(f"Usage: {sys.argv[0]} <libnet80211.a> <objcopy> <ar> <output.a>")
        sys.exit(1)

    lib_in, objcopy, ar, lib_out = sys.argv[1:5]
    objdump = objcopy.replace("objcopy", "objdump")

    with tempfile.TemporaryDirectory() as tmpdir:
        # Copy archive and extract the target .o
        lib_work = os.path.join(tmpdir, "libnet80211.a")
        shutil.copy2(lib_in, lib_work)

        obj_name = "ieee80211_output.o"
        subprocess.check_call([ar, "x", lib_work, obj_name], cwd=tmpdir)
        obj_path = os.path.join(tmpdir, obj_name)

        # Find section file offset
        sec_off, sec_size = find_section_offset(objdump, obj_path, FUNC_SECTION)
        if sec_off is None:
            print(f"ERROR: section {FUNC_SECTION} not found")
            sys.exit(1)

        # Find function entry within section
        func_off = find_func_offset_in_section(objdump, obj_path, FUNC_SECTION)
        if func_off is None:
            print(f"ERROR: entry instruction not found in {FUNC_SECTION}")
            sys.exit(1)

        file_off = sec_off + func_off
        patch = PATCH_ENTRY + PATCH_BODY

        # Check if already patched or needs patching
        with open(obj_path, "rb") as f:
            f.seek(file_off)
            existing = f.read(len(PATCH_ENTRY) + len(PATCH_BODY))

        if existing == PATCH_ENTRY + PATCH_BODY:
            print(f"Already patched at 0x{file_off:x}, skipping")
            shutil.copy2(lib_work, lib_out)
            return

        if existing[:3] != PATCH_ENTRY:
            print(f"ERROR: expected entry a1,64 ({PATCH_ENTRY.hex()}) at offset "
                  f"0x{file_off:x}, got {existing[:3].hex()}")
            sys.exit(1)

        # Apply patch
        with open(obj_path, "r+b") as f:
            f.seek(file_off)
            f.write(patch)

        print(f"Patched {FUNC_SECTION} at file offset 0x{file_off:x} "
              f"(section 0x{sec_off:x} + 0x{func_off:x}): "
              f"{existing[:3].hex()}... -> {patch.hex()}")

        # Replace .o in archive
        subprocess.check_call([ar, "r", lib_work, obj_name], cwd=tmpdir)

        shutil.copy2(lib_work, lib_out)
        print(f"Written patched library to {lib_out}")


if __name__ == "__main__":
    main()

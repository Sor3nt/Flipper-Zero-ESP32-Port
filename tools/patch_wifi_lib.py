#!/usr/bin/env python3
"""
Patch ESP-IDF's libnet80211.a to allow raw TX of deauth/disassoc frames.

The closed-source WiFi blob's ieee80211_raw_frame_sanity_check() rejects
management frame subtypes 0xA0 (disassoc) and 0xC0 (deauth). This script
patches the function body in ieee80211_output.o to return 0 (ESP_OK)
immediately, then re-archives it.

The patch overwrites the function entry with:
  entry a1,64   ; movi.n a2,0   ; retw.n

The remaining bytes in the section are NOP-filled and the relocation section
is stripped to prevent the linker from choking on stale relocations in the
dead code area.
"""
import sys, subprocess, shutil, tempfile, os, re

FUNC_SECTION = ".text.ieee80211_raw_frame_sanity_check"
RELA_SECTION = ".rela" + FUNC_SECTION

# Xtensa patch: entry a1,64 ; movi.n a2,0 ; retw.n
PATCH_ENTRY = bytes([0x36, 0x81, 0x00])  # entry a1, 64 (keep)
PATCH_BODY  = bytes([0x0c, 0x02, 0x1d, 0xf0])  # movi.n a2, 0; retw.n
PATCH       = PATCH_ENTRY + PATCH_BODY  # 7 bytes total

# Xtensa nop.n = 0x3d0f (big-endian) -> 0f 3d in LE byte stream
NOPN = bytes([0x0f, 0x3d])


def find_section_info(objdump, obj_path, section_name):
    """Find file offset and size of a section in an ELF .o file."""
    out = subprocess.check_output([objdump, "-h", obj_path], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 7 and parts[1] == section_name:
            size = int(parts[2], 16)
            file_off = int(parts[5], 16)
            return file_off, size
    return None, None


def find_sections(objdump, obj_path):
    """Return dict mapping section name -> (file_offset, size)."""
    out = subprocess.check_output([objdump, "-h", obj_path], text=True)
    sections = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 7 and re.match(r'^\.', parts[1]):
            sections[parts[1]] = (int(parts[5], 16), int(parts[2], 16))
    return sections


def find_func_offset(objdump, obj_path, section_name):
    """Find the function entry offset within its section (first 'entry' insn)."""
    out = subprocess.check_output([objdump, "-d", "-j", section_name, obj_path], text=True)
    for line in out.splitlines():
        if "entry" in line and ":" in line:
            addr_str = line.strip().split(":")[0].strip()
            return int(addr_str, 16)
    return None


def nop_fill(length):
    """Fill a buffer of length bytes with nop.n instructions (2-byte narrow nops)."""
    nops = bytearray(length)
    for i in range(0, length - 1, 2):
        nops[i:i + 2] = NOPN
    # If odd length, pad with a single 0 byte (won't be reached)
    if length % 2 == 1:
        nops[-1] = 0
    return bytes(nops)


def main():
    if len(sys.argv) != 5:
        print(f"Usage: {sys.argv[0]} <libnet80211.a> <objcopy> <ar> <output.a>")
        sys.exit(1)

    lib_in, objcopy, ar, lib_out = sys.argv[1:5]
    objdump = objcopy.replace("objcopy", "objdump")

    with tempfile.TemporaryDirectory() as tmpdir:
        lib_work = os.path.join(tmpdir, "libnet80211.a")
        shutil.copy2(lib_in, lib_work)

        obj_name = "ieee80211_output.o"
        subprocess.check_call([ar, "x", lib_work, obj_name], cwd=tmpdir)
        obj_path = os.path.join(tmpdir, obj_name)

        # Find section info
        sec_off, sec_size = find_section_info(objdump, obj_path, FUNC_SECTION)
        if sec_off is None:
            print(f"ERROR: section {FUNC_SECTION} not found")
            sys.exit(1)

        # Find function entry within section
        func_off = find_func_offset(objdump, obj_path, FUNC_SECTION)
        if func_off is None:
            print(f"ERROR: entry instruction not found in {FUNC_SECTION}")
            sys.exit(1)

        patch_file_off = sec_off + func_off

        # Check if already patched
        with open(obj_path, "rb") as f:
            f.seek(patch_file_off)
            existing = f.read(len(PATCH))

        if existing == PATCH:
            print(f"Already patched at 0x{patch_file_off:x}, skipping")
            shutil.copy2(lib_work, lib_out)
            return

        if existing[:3] != PATCH_ENTRY:
            print(f"ERROR: expected entry a1,64 ({PATCH_ENTRY.hex()}) at offset "
                  f"0x{patch_file_off:x}, got {existing[:3].hex()}")
            sys.exit(1)

        # --- Apply patch ---
        with open(obj_path, "r+b") as f:
            # Write the early-return patch
            f.seek(patch_file_off)
            f.write(PATCH)

            # NOP-fill the rest of the section (dead code after retw.n)
            after_patch = func_off + len(PATCH)
            nop_len = sec_size - after_patch
            if nop_len > 0:
                f.write(nop_fill(nop_len))

        print(f"Patched {FUNC_SECTION} at file offset 0x{patch_file_off:x}: "
              f"{existing[:3].hex()}... -> {PATCH.hex()}, "
              f"NOP-filled {nop_len} bytes of dead code")

        # --- Strip the relocation section for the patched function ---
        # The dead-code relocations are now stale; strip them so the linker
        # doesn't try to decode corrupted instructions.
        all_sections = find_sections(objdump, obj_path)
        if RELA_SECTION in all_sections:
            rela_off, rela_size = all_sections[RELA_SECTION]
            print(f"Stripping {RELA_SECTION} (offset 0x{rela_off:x}, size {rela_size})")
            # Zero out the relocation entries by overwriting the section data
            with open(obj_path, "r+b") as f:
                f.seek(rela_off)
                f.write(b'\x00' * rela_size)
        else:
            print(f"No {RELA_SECTION} found, nothing to strip")

        # Replace .o in archive
        subprocess.check_call([ar, "r", lib_work, obj_name], cwd=tmpdir)

        shutil.copy2(lib_work, lib_out)
        print(f"Written patched library to {lib_out}")


if __name__ == "__main__":
    main()

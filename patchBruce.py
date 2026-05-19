#!/usr/bin/env python3
"""Keep the bundled Bruce checkout up to date and apply the multi-boot patch.

What it does (idempotent, safe to run before every build):
  1. clones BruceDevices/firmware into multi-boot/bruce if it's missing
  2. resets the working tree to a pristine state
  3. `git pull --ff-only` so a build always picks up upstream changes
  4. re-applies tools/bruce_multiboot.patch (adds the "Flipper Zero" main-menu
     entry that reboots into the ota_0 slot — see 00_Skills/multi-boot.md)
  5. copies partitions_multiboot.csv over Bruce's custom_16Mb.csv so both
     firmwares are built against the exact same partition table

Exits non-zero (loudly) if the patch no longer applies — that means upstream
Bruce moved the menu code and tools/bruce_multiboot.patch must be regenerated.
"""

import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
BRUCE_DIR = REPO_ROOT / "multi-boot" / "bruce"
BRUCE_REPO_URL = "https://github.com/m0lomalteser/firmware.git"
PATCH_FILE = REPO_ROOT / "tools" / "bruce_multiboot.patch"
PARTITIONS_SRC = REPO_ROOT / "partitions_multiboot.csv"
PARTITIONS_DST_NAME = "custom_16Mb.csv"

# Files that tools/bruce_multiboot.patch *creates* (as opposed to modifies).
# `git reset --hard` won't remove these, so we delete them explicitly before
# re-applying the patch to keep the operation idempotent.
PATCH_CREATED_FILES = [
    "src/core/menu_items/FlipperOsMenu.h",
    "src/core/menu_items/FlipperOsMenu.cpp",
]


def run(cmd):
    print("+ " + " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def git(*args, check=True):
    cmd = ["git", "-C", str(BRUCE_DIR), *args]
    print("+ " + " ".join(cmd))
    return subprocess.run(cmd, check=check)


def reset_worktree():
    git("reset", "--hard", "HEAD")
    for rel in PATCH_CREATED_FILES:
        path = BRUCE_DIR / rel
        if path.exists():
            print(f"  rm {rel}")
            path.unlink()


def main():
    if not PATCH_FILE.is_file():
        sys.exit(f"error: missing patch file: {PATCH_FILE}")
    if not PARTITIONS_SRC.is_file():
        sys.exit(f"error: missing partition table: {PARTITIONS_SRC}")

    if not (BRUCE_DIR / ".git").is_dir():
        if BRUCE_DIR.exists():
            if any(BRUCE_DIR.iterdir()):
                sys.exit(
                    f"error: {BRUCE_DIR} exists but is not a git checkout. "
                    "Remove it and rerun, or run patchBruce.py manually."
                )
            BRUCE_DIR.rmdir()  # leftover empty dir — git clone wants it gone
        print(f"Bruce checkout not found, cloning into {BRUCE_DIR} ...")
        BRUCE_DIR.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--depth", "1", BRUCE_REPO_URL, str(BRUCE_DIR)])

    # 1) pristine tree
    reset_worktree()

    # 2) keep Bruce current
    if git("pull", "--ff-only", check=False).returncode != 0:
        print(
            "warning: 'git pull' failed (offline / non-ff?), continuing with the "
            "local Bruce checkout",
            file=sys.stderr,
        )
        reset_worktree()

    # 3) apply the multi-boot menu patch
    if git("apply", "--whitespace=nowarn", str(PATCH_FILE), check=False).returncode != 0:
        sys.exit(
            "\nerror: tools/bruce_multiboot.patch did not apply.\n"
            "Upstream Bruce most likely changed src/core/main_menu.{h,cpp}.\n"
            "Regenerate the patch — see 00_Skills/multi-boot.md ('Updating the "
            "Bruce patch').\n"
        )

    # 4) single-source the partition table
    shutil.copyfile(PARTITIONS_SRC, BRUCE_DIR / PARTITIONS_DST_NAME)
    print(f"copied {PARTITIONS_SRC.name} -> multi-boot/bruce/{PARTITIONS_DST_NAME}")

    print("Bruce checkout is patched and ready for multi-boot.")


if __name__ == "__main__":
    main()

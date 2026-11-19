# USB CD-ROM — ESP32 port notes

Upstream: https://github.com/piersroberts/flippercd (GPL-3.0, commit 9efbba8), a
sister app to USB Floppy forked from the official Mass Storage app. Emulates a
USB CD/DVD-ROM drive from an `.iso` image on the SD card (read-only).

## Approach (same split as usb_floppy)
Upstream ships its own `FuriHalUsbInterface` + BOT engine + a full SCSI/MMC
parser (`helpers/scsi_cd.c`). None of that layer is portable here (fixed TinyUSB
composite, opaque interface). Instead:
- **SBC commands** are TinyUSB's built-ins: TEST UNIT READY, READ CAPACITY(10)
  → 2048-byte blocks from `tud_msc_capacity_cb`, READ(10) → the backing. TinyUSB
  derives the read block size from the host's own transfer length
  (`rdwr10_get_blocksize` = total_bytes / block_count), so 2048-byte stepping is
  automatic.
- **MMC commands** that TinyUSB doesn't know (READ TOC 0x43, GET CONFIGURATION
  0x46, GET EVENT STATUS NOTIFICATION 0x4A, READ DISC INFORMATION 0x51, READ
  TRACK INFORMATION 0x52, MODE SENSE(10) 0x5A, MECHANISM STATUS 0xBD, plus a few
  OK-only commands) are implemented in the firmware's `tud_msc_scsi_cb`, which
  TinyUSB calls for any command `proc_builtin_scsi` returns <0 for. Response
  bytes follow MMC-5 and mirror flippercd.
- **INQUIRY device type 0x05** is set through TinyUSB's weak `tud_msc_inquiry2_cb`
  (its built-in INQUIRY hardcodes 0x00). The override returns 0 for non-CD
  backings, so the SD USB-stick and the floppy keep type 0x00 — no regression.

All of the above lives in `components/furi_hal/furi_hal_usb_msc.c`, gated on the
active backing's `is_cdrom` flag. The FAP just sets `is_cdrom=true`,
`block_size=2048`, `writable=false` and supplies the read callback.

A disc larger than ~703 MB is advertised with the DVD-ROM profile (0x0010),
otherwise CD-ROM (0x0008); either way READ CAPACITY reports the real size and the
host mounts the ISO9660 filesystem.

## USB handling
Identical lease flow to usb_floppy: `lease_acquire` → `composite_install_for_owner`
→ `furi_hal_usb_msc_start_backing` (read-only, SD stays mounted) → `reinit_for_owner`
→ light-sleep veto; released in reverse (`msc_stop` → 1.5 s wait → `composite_uninstall`
→ `lease_release` → drop veto), retried 5×100 ms. "USB is busy" when refused.

## Reads
The backing read runs on the TinyUSB task and serves `count × 2048` bytes from the
ISO via `storage_file_seek`/`read` (offset = LBA × 2048). The ISO must be a
multiple of 2048 B and under 4 GB (32-bit file offset). Only the TinyUSB task
touches the open `File*` while mounted.

## Known limitations
- Reads go through READ(10) only (the standard path for data CDs). READ(12)
  (0xA8) and READ CD (0xBE) return INVALID COMMAND — hosts fall back to READ(10).
- Single data track, Mode 1, 2048-byte user data. No audio CDs, no multisession.
- DVD structure (READ DISC STRUCTURE 0xAD) is not implemented; a DVD-sized ISO
  still mounts as a large data volume.

## Input / UI / board support
ViewPort state machine: Select (OK = browse `.iso`, Back = exit), Work (name, size,
"read-only", bytes read; Back = stop, with an "eject first" warning when the host
holds the medium; long Back forces it). Board gate:
`furi_hal_board_supports_ported_apps()` (T-Embed, Paradoxt, ThorChimera, Thormini).

## Using it
Put an `.iso` in `apps_data/usb_cdrom/` on the SD, launch USB CD-ROM, pick it, and
the PC should mount it as a read-only optical drive.

## Fixes 2026-11-15
- **Error screen now says which step failed.** Every failure used to end on a bare "Mount failed"/
  "Failed". `app->error_reason` is set at each failure point and drawn under the title:
  `Cannot open file` (storage open), `USB composite install`
  (`furi_hal_usb_composite_install_for_owner`), `MSC start` (`furi_hal_usb_msc_start_backing`).
  A refused lease still shows the separate "USB is busy" screen. Each failure is also logged.
- Audited for the "won't start" report: the API table on the firmware side is valid (1256 entries,
  sorted, hashes correct, all four `furi_hal_usb_msc_*` + composite exports present), the FAP
  manifests carry the same API version as the working HID apps, the deployed `.fap` is identical to
  the current build, the shared HAL path matches the built-in USB-storage scene, and the backing
  callbacks (TinyUSB task -> storage API) need no FuriThread. **Root cause still unconfirmed on
  hardware** -- the on-screen reason above is what is needed next.

## Loader finding (2026-11-15, user report: "opens, then straight back to the menu")
This firmware's loader never showed a start error: `loader_start_with_gui_error()` and the
detached path only logged `Start error: ...` and returned to the menu, which is exactly the
reported symptom. Firmware fix (needs reflash): `components/loader/loader.c` now shows a
"Cannot start app" dialog with the reason, and the ELF loader records its first concrete
cause (`elf_file_get_last_error()`: e.g. `Missing: <symbol>`, `Reloc: L32R range ...`,
`Section <name>`, `Out of memory`), appended to the message. Note that any relocation failure is
reported by the loader as "Missing imports". Statically verified identical to the working HID
apps: API table entries, manifest/API version, stack, relocation types, ELF sections, e_entry.

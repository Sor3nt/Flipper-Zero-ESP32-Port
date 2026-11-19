# Mass Storage — ESP32 port notes

Upstream: https://github.com/flipperdevices/flipperzero-good-faps (dev branch,
`mass_storage`). Exposes a disk image (`.img`) on the SD as a USB mass storage
device, and can create blank images.

## Approach (same backing layer as usb_floppy)
The upstream ships its own `FuriHalUsbInterface` + BOT engine + SCSI parser
(`helpers/mass_storage_usb.c`, `mass_storage_scsi.c`). That is not portable to
this firmware's fixed TinyUSB composite. Instead this reuses the shared MSC
backing added for usb_floppy: `furi_hal_usb_msc_start_backing()` with a
**writable** 512-byte file backing. TinyUSB does the BOT transport + standard
SCSI (INQUIRY type 0x00, READ CAPACITY, READ10/WRITE10, MODE SENSE, …); the FAP
only maps LBA → file offset. INQUIRY strings are "Flipper" / "Mass Storage" /
"0001" to match the upstream. No firmware change was needed beyond the backing
layer floppy already added.

## USB handling
Identical lease flow to usb_floppy: `lease_acquire` → `composite_install_for_owner`
→ `furi_hal_usb_msc_start_backing` (read/write; SD stays mounted) →
`reinit_for_owner` → light-sleep veto; released in reverse with the 1.5 s host
settle and 5×100 ms retries. "USB is busy" when refused.

## Data path
The backing read/write run on the TinyUSB task and go to the open `.img` via
`storage_file_seek`/`read`/`write`; every write is `storage_file_sync`'d so Back
(or unplug) does not lose data the host believes is committed. Only the TinyUSB
task touches the open `File*` while mounted.

## Image creation
"New image" picks a preset size (1.44M … 256M), creates
`apps_data/mass_storage/new_<size>.img` (a free name, `_N` appended if taken),
expands it to full size (FatFs allocates the clusters — no sparse files, so big
images take time and real SD space; make larger ones on a PC), and zeroes the
first 4 KiB so the host sees a fresh, unformatted drive to format itself.

## Validation / limits
Selected images must be a non-zero multiple of 512 B and under 4 GB (32-bit file
offset). Anything else → "Bad image".

## Input / UI / board support
ViewPort state machine: Select (Up/Down choose Select-image vs New-image, `<>`
change the new size, OK acts, Back exits), a "Creating image..." screen while a
new image is built, then the Work screen (name, size, R/W KiB; Back stops, with
the "eject first" warning + long-Back force). Board gate:
`furi_hal_board_supports_ported_apps()` (T-Embed, Paradoxt, ThorChimera, Thormini).

## Fixes 2026-11-15
- **Error screen now says which step failed.** Every failure used to end on a bare "Mount failed"/
  "Failed". `app->error_reason` is set at each failure point and drawn under the title:
  `Cannot open file` (storage open), `USB composite install`
  (`furi_hal_usb_composite_install_for_owner`), `MSC start` (`furi_hal_usb_msc_start_backing`), `Create image` (new-image path).
  A refused lease still shows the separate "USB is busy" screen. Each failure is also logged.
- Audited for the "won't start" report: the API table on the firmware side is valid (1256 entries,
  sorted, hashes correct, all four `furi_hal_usb_msc_*` + composite exports present), the FAP
  manifests carry the same API version as the working HID apps, the deployed `.fap` is identical to
  the current build, the shared HAL path matches the built-in USB-storage scene, and the backing
  callbacks (TinyUSB task -> storage API) need no FuriThread. **Root cause still unconfirmed on
  hardware** -- the on-screen reason above is what is needed next.
- Encoder: the new-image size (`<>` on the D-pad) is OK held + turn; the hint reads `OK+turn`.

## Loader finding (2026-11-15, user report: "opens, then straight back to the menu")
This firmware's loader never showed a start error: `loader_start_with_gui_error()` and the
detached path only logged `Start error: ...` and returned to the menu, which is exactly the
reported symptom. Firmware fix (needs reflash): `components/loader/loader.c` now shows a
"Cannot start app" dialog with the reason, and the ELF loader records its first concrete
cause (`elf_file_get_last_error()`: e.g. `Missing: <symbol>`, `Reloc: L32R range ...`,
`Section <name>`, `Out of memory`), appended to the message. Note that any relocation failure is
reported by the loader as "Missing imports". Statically verified identical to the working HID
apps: API table entries, manifest/API version, stack, relocation types, ELF sections, e_entry.

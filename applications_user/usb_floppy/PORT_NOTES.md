# USB Floppy — ESP32 port notes

Upstream: https://github.com/piersroberts/flipperfloppy (GPL-3.0), a fork of the
official Mass Storage app. Emulates a USB floppy drive from a 720K or 1.44M disk
image (`.img`) kept on the SD card.

## Why this is not a line-by-line port
The upstream app ships its **own** USB device: it defines a full
`FuriHalUsbInterface` with custom device/config descriptors and drives the
endpoints itself (UFI subclass, CBI/Bulk-Only transport, a hand-written SCSI/UFI
command parser in `helpers/ufi.c`). That whole layer depends on the STM32
`usbd_*` stack and a rich `FuriHalUsbInterface`.

Neither exists here. This firmware exposes a **fixed TinyUSB composite
(HID+CDC+MSC)**; `FuriHalUsbInterface` is an opaque `{ const char* name; }` and
apps cannot add descriptors. TinyUSB already implements the Bulk-Only transport
and the standard SCSI command set (INQUIRY, READ CAPACITY, READ10/WRITE10, MODE
SENSE, TEST UNIT READY, PREVENT/ALLOW, REQUEST SENSE) through the firmware's
`tud_msc_*_cb` callbacks. So the UFI/BOT/SCSI code is **not** ported — it would
duplicate what TinyUSB does.

What the port actually needs is only the *backing store*: map an LBA to a file
offset. So the firmware's MSC layer was made pluggable (see below) and this app
supplies a file-backed read/write.

Functional note: the host sees a **removable SCSI block device** of 2880 (or
1440) × 512 bytes, not a true UFI "floppy". For serving a FAT12/16 image that is
equivalent — the host mounts whatever filesystem is in the `.img`.

## Firmware change (shared with usb_cdrom and mass_storage)
`components/furi_hal/furi_hal_usb_msc.{h,c}` gained a backing abstraction:
`FuriHalUsbMscBacking { block_size, block_count, writable, is_cdrom, INQUIRY
strings, read(), write(), eject() }` and `furi_hal_usb_msc_start_backing()`.
When a backing is active the `tud_msc_*_cb` callbacks dispatch to it; otherwise
they use the raw SD path as before. Unlike `furi_hal_usb_msc_start()` (raw SD),
the backing path does **not** unmount the SD — the image is read *through* the
mounted filesystem. Four symbols were exported to FAPs in
`flipper_application/firmware_api.c` (start_backing, stop, is_active,
is_removal_locked).

`is_cdrom` is carried but unused here (floppy = direct-access disk); it is for
the later usb_cdrom port, which also needs INQUIRY device-type 0x05 + MMC
commands in `tud_msc_scsi_cb`.

## USB port handling
Same lease flow as `pc_monitor_usb` / `mouse_click_recorder` (see their
PORT_NOTES), adapted for MSC:
1. `furi_hal_usb_lease_acquire` — exclusive; refused while USB is locked (RPC),
   leased (UART bridge) or used by USB storage → the screen shows "USB is busy".
2. `furi_hal_usb_composite_install_for_owner`.
3. `furi_hal_usb_msc_start_backing` (file-backed; SD stays mounted).
4. `furi_hal_usb_reinit_for_owner` + light-sleep veto.
Release is the reverse: `furi_hal_usb_msc_stop` → wait 1.5 s (host notices
MEDIUM_NOT_PRESENT) → `composite_uninstall_for_owner` → `lease_release` → drop
the sleep veto. Retried 5×100 ms at exit; a failed release keeps what is still
owned. The upstream `furi_hal_usb_set_config(&usb, ...)` / reset-on-exit is gone.

## Data path and threading
The backing `read`/`write` run on the **TinyUSB task** and reach the image with
`storage_file_seek`/`read`/`write` through the mounted FS. Only the TinyUSB task
touches the open `File*` while MSC is active (the app opens it before acquiring
USB and closes it only after USB is fully released), so no extra lock is needed.
Each write is `storage_file_sync`'d so pressing Back (or unplugging) does not
drop data the host believes is committed. Writes are refused when write-protect
is on (`is_writable_cb` + the write callback both check it).

## Input / UI (stock stream, ViewPort state machine)
Select screen: `<>`/`^v` toggle write-protect, OK opens the file browser
(`.img`, app-data folder), Back exits. A chosen image is validated to be exactly
737280 B (720K) or 1474560 B (1.44M); anything else → "Not a floppy image".
Work screen shows name, geometry, R/W KiB; Back stops. If the host still holds
the medium (SCSI PREVENT), Back first warns "Eject first"; a long Back forces it.

## Board support
Supported boards only (`furi_hal_board_supports_ported_apps()`): T-Embed,
Paradoxt, ThorChimera, Thormini.

## Using it
Put a raw 720K/1.44M image named `*.img` in `apps_data/usb_floppy/` on the SD
(create one on a PC, or `dd if=/dev/zero bs=512 count=2880` then FAT12-format).

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
- Encoder: any wheel turn toggles write-protect on the Select screen; the hint reads `turn=WP`.

## Loader finding (2026-11-15, user report: "opens, then straight back to the menu")
This firmware's loader never showed a start error: `loader_start_with_gui_error()` and the
detached path only logged `Start error: ...` and returned to the menu, which is exactly the
reported symptom. Firmware fix (needs reflash): `components/loader/loader.c` now shows a
"Cannot start app" dialog with the reason, and the ELF loader records its first concrete
cause (`elf_file_get_last_error()`: e.g. `Missing: <symbol>`, `Reloc: L32R range ...`,
`Section <name>`, `Out of memory`), appended to the message. Note that any relocation failure is
reported by the loader as "Missing imports". Statically verified identical to the working HID
apps: API table entries, manifest/API version, stack, relocation types, ELF sections, e_entry.

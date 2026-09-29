# T-Embed and BW16 setup

## Choose the correct hardware build

| Display name | Hardware | Matching custom I2C bundle |
| --- | --- | --- |
| **Lilygo T-embed cc1101** | Without NRF connected to GPIO43/44 | `T-Embed-NonNRF-I2C-WDGWars-1.3` |
| **Lily T-embed cc1101 plus** | Plus with NRF installed | `T-Embed-CC1101-Plus-SD-Recovery-1.3.2` |

The T-Embed images are different: the non-NRF profile retains GPS UART43/44;
the Plus profile reserves those pins for NRF. Do not interchange host images.
Both listed bundles contain the **same BW16 I2C 4.5 firmware**. The separate
ProtoPirate RX fix uses the same replacement FAP on both.

**Availability and scope:** the I2C bundles above are separately distributed
custom builds, not official upstream releases or binary attachments to this PR.
Their flashing scripts/images are not generated merely by checking out this PR.
The commands below apply only after obtaining and extracting one of those complete
bundles. This PR's existing BW16 UART controller is documented separately in
[BW16-DEPLOYMENT.md](BW16-DEPLOYMENT.md); it is not interchangeable with the I2C
controller or this wiring. Do not mix UART and I2C instructions.

## Important: connect BW16 only with the Lilygo fully powered off

On the tested Plus, plugging BW16 into a running Lilygo caused an immediate reboot
and an SD mount failure. Fully powering off, connecting BW16, then starting with
a long press of the power button worked: both the SD card and BW16 were usable.
Leaving BW16 connected and physically reinserting the SD also restored operation,
but is not the recommended routine. SD software retries and delayed BW16 Wi-Fi
startup did not prevent the hot-plug failure. Its electrical cause has not been
measured. Do not describe hot-plugging as supported or fixed.

**Deep Sleep is not full Power Off.** Configure shutdown this way:

1. Stop any current operation and exit the app.
2. Disconnect the Lilygo USB cable and any separately powered external module.
3. Open **Settings > Power > Off Mode** and select **Power Off**, not Deep Sleep.
4. Exit Settings completely so the power service receives the new setting.
5. Reopen **Settings > Power > Power OFF** and confirm.
6. With the board off and SD inserted, connect BW16 at the Lilygo connector.
7. Start using the power/QON button (long press worked on the tested Plus).
8. Confirm the SD mounts, then open **GPIO > BW16 I2C** and run a scan.

The firmware requests battery-controller ship mode for real shutdown. USB power
can keep the system powered; this firmware falls back to deep sleep when USB is
present. Unplugging USB must therefore happen **before** requesting full Power Off.
A dark display alone is not proof of a power cut. The normal encoder wake from
deep sleep is also not evidence that the supply was removed.

Use this same power-off/connect/start sequence on the non-NRF build. Fully power
off before disconnecting or changing the module wiring as well. The NRF can remain
installed. No desoldering is needed to disconnect the cable at the Lilygo end.
If a board revision cannot wake via its QON button, connecting Lilygo USB is the
alternative wake method; keep BW16 USB disconnected.

## Confirmed four-wire I2C connection

Power off before moving wires. Pin labels are authoritative; colours below refer
to this particular cable and supersede older yellow/white descriptions.

| Wire | T-Embed, either I2C profile | BW16-KIT |
| --- | --- | --- |
| **White** | GPIO8 / SDA | PA26 / D8 / SDA |
| **Yellow** | GPIO18 / SCL | PA25 / D7 / SCL |
| **Red** | 3.3 V | 3V3 |
| **Black** | GND | GND |

I2C uses address `0x42`, 100 kHz and 3.3 V logic. Connect SDA to SDA and SCL to SCL;
there is no TX/RX crossing. PA7/TX0 and PB2/RX1 belong to the previous UART setup.
Do not put 5 V on 3V3. GPIO43/44 are NRF control pins on the Plus and must not be
repurposed for BW16/GPS UART while NRF is installed.

**Never connect BW16 USB while its red supply wire is connected to Lilygo 3.3 V.**
Disconnect the four-wire plug at the Lilygo end before flashing BW16 through USB.
Remove BW16 USB before reconnecting that plug. This keeps the two supplies isolated.

## Flash the T-Embed

Back up the SD card, settings and any known-working firmware bundle. Stop apps,
fully power off, and unplug BW16 at the Lilygo connector. Connect only Lilygo USB.
Extract the correct bundle and open PowerShell in its top-level directory (the one
containing `T-Embed` and `BW16`). COM4 below is an example; use the Lilygo's current
port in Device Manager. Close serial monitors before flashing.

```powershell
powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM4
```

The script needs a Python executable containing esptool. If the bundled default
ESP-IDF Python location does not exist on your PC, specify your actual executable:

```powershell
powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM4 -Python "C:\path\to\python.exe"
```

To verify the bundle without opening hardware:

```powershell
powershell -ExecutionPolicy Bypass -File .\T-Embed\FLASH.ps1 -Port COM4 -VerifyOnly
```

The script checks hashes and uses the bundle's `flash_args` to write bootloader,
partition table, application and OTA metadata at the matching offsets. It does
not format the SD or perform a whole-chip erase. Do not mix files from profiles.
If auto-reset cannot enter the loader, hold BOOT/encoder, press/release RST, then
release BOOT and use the newly enumerated COM port. A failed write or checksum
check is not a successful installation; keep the known-working bundle.

## Flash BW16

A module already running the matching **I2C 4.5** image needs no reflash for a
T-Embed update or the ProtoPirate fix. UART firmware does require replacement when
moving to the I2C setup. The experimental 4.5.1 startup-delay image did not cure
hot-plugging; it does not remove the full-power-off requirement.

With BW16 disconnected from Lilygo, connect BW16 USB alone. From the same extracted
bundle root, using the BW16's port (COM3 is an example):

```powershell
powershell -ExecutionPolicy Bypass -File .\BW16\FLASH_BW16.ps1 -Port COM3 -BaudRate 460800
```

Follow the uploader prompt: hold **BURN**, press and release **RST**, then release
**BURN**. Wait for **SUCCESS** and the verified boot/application checksum result.
The initial handshake uses 115200 even when the selected transfer baud is 460800.
After verified success, press RST and unplug BW16 USB. Reconnect only after fully
powering off Lilygo as described above.

For an offline image/uploader check:

```powershell
powershell -ExecutionPolicy Bypass -File .\BW16\FLASH_BW16.ps1 -CheckOnly
```

If uploading stalls or fails, close other serial tools, re-enter BURN/RST upload
mode and retry. `-BaudRate 115200` is an available fallback, not a guarantee of
success. Preserve the uploader log and never treat partial progress as verified.

## SD-card files and app updates

For a fresh installation, copy the **contents** of `T-Embed/SD/` into the SD root;
do not create an extra `T-Embed/SD` directory on the card. For upgrades, back up
first and merge apps carefully, preserving existing `apps_data` credentials,
settings, logs, captures and offline map files. Do not format a working card just
because it fails to mount after a hot-plug.

| Application | SD path |
| --- | --- |
| BW16 I2C | `apps/GPIO/bw16_r4tkn_i2c.fap` |
| Wardriver, matching hardware profile | `apps/Tools/wardriver.fap` |
| WDGWars Map | `apps/Tools/wdgwars_map.fap` |
| ProtoPirate RX fix | `apps/SubGhz/proto_pirate.fap` |

For the ProtoPirate fix only, exit the app, back up and replace that one FAP. Keep
its existing assets/plugins and saved captures. No host or BW16 flash is needed.
See [ProtoPirate receive stability](applications_user/protopirate/RX-STABILITY.md).

The old `apps/GPIO/bw16_r4tkn.fap` is a UART controller. Back it up outside the
active app directory when migrating to I2C; merging folders does not remove it.
Open **BW16 I2C**, not the old UART app. Safely eject USB storage/card before using
apps. Close SD-using apps before entering USB storage mode.

WDGWars credentials stay in `apps_data/wardriver/wdgwars.conf`:

```text
API Key: YOUR_64_CHARACTER_WDGWARS_API_KEY
```

Preserve existing credentials instead of replacing them with this placeholder.
Offline map areas remain in `apps_data/wdgwars_map/area1.wdg` through `area4.wdg`.
The map and Wardriver remain separate apps.

## Troubleshooting

| Symptom | Check / next step |
| --- | --- |
| Reboot and SD mount failure immediately after connecting BW16 | Use full Power Off with USB removed, connect BW16, then start. Deep Sleep is insufficient. |
| SD still fails | Leave wiring stable; try a true power cycle. If necessary, fully power off, reseat the card, and start again. Preserve data and capture a boot log if it persists. Software retries do not reproduce physical SD power removal. |
| `RX=0` / `No UART reply` in the I2C setup | Check the app name: the old UART FAP is likely open. Both host and BW16 must have matching I2C firmware. |
| `I2C peer not ready`, timeout `0x107` | With power off, check SDA8 to PA26, SCL18 to PA25, 3.3 V and common GND; verify matching BW16 firmware and restart with the module already connected. |
| COM port busy / access denied | Close monitors, other flashers and terminal apps; check the current port after reconnecting. |
| Cannot configure port / USB device not functioning | Reconnect with a data-capable cable, re-enter loader mode, and check the re-enumerated port. |
| BW16 upload failed partway | Re-enter upload mode and rerun until checksums are verified; retain the log and recovery image. |
| ProtoPirate still resets | Verify the replacement FAP path and record a new crash log. The RX fix does not claim to fix every reset or the separate boot-time SPI assertion. |

The non-NRF build can retain GPS on RX44/TX43; the Plus cannot use those pins for
GPS with NRF installed. This guide does not add simultaneous app execution or
promise GPS/BW16 concurrency. Select the appropriate application for the task.

For diagnostics, capture Lilygo's USB console at 115200 through the failure/reboot,
record the hardware profile and whether BW16 was connected before startup, and
include the assertion/backtrace. Close the monitor before flashing. Review logs
for credentials and personal network/device details before posting them publicly.

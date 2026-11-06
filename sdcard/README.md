# SD card contents

> **Important:** Keep the original SD card ZIP unchanged. Retain its existing files, replace the old U2F pair in `/u2f/assets/`, replace/add only the affected FAPs listed below, and add their required support files. Leave unrelated files unchanged.

This directory is a partial overlay of SD card files to add or replace; it is not a complete replacement for the original SD card ZIP. Leave all other files from the original card as they are.

## Included files

| Repository path | Card path | Purpose |
|---|---|---|
| `apps/GPIO/` | `/apps/GPIO/` | BW16 R4TKN, BW16 I2C, and I2C Tools CLI FAPs. |
| `apps/GPIO/esp32/` | `/apps/GPIO/esp32/` | ESP32 WiFi Marauder companion FAP. |
| `apps/Infrared/` | `/apps/Infrared/` | FlipIRFreq FAP. |
| `apps/SubGhz/` | `/apps/SubGhz/` | FMTX, ProtoPirate, POCSAG Pager, Spectrum Analyzer, and Sub-GHz Raw Edit FAPs. |
| `apps/USB/` | `/apps/USB/` | Mouse Click Recorder and PC Monitor USB FAPs. |
| `apps_assets/fmtx/` | `/apps_assets/fmtx/` | MP3 tracks required by the FMTX FAP. |
| `apps_assets/proto_pirate/` | `/apps_assets/proto_pirate/` | ProtoPirate plugin FALs, model database, and keystore data. |
| `apps_data/js_app/plugins/js_badusb.fal` | `/apps_data/js_app/plugins/js_badusb.fal` | Updated JavaScript BadUSB plugin; replace the older file at this path. |
| `asset_packs/` | `/asset_packs/` | 35 animation packs, with their manifests and frame files. Select one in Settings → Interface → Graphics → Asset Pack. |
| `u2f/assets/` | `/u2f/assets/` | The matching `cert.der` and `cert_key.u2f` pair required to provision U2F. On first use, firmware imports and encrypts the key. |

### Replace the previous U2F files

If the SD card already has `cert.der` and `cert_key.u2f` under `/u2f/assets/`, replace **both files together** with the updated pair from this repository's `sdcard/u2f/assets/` directory. The previous pair failed in the device testing; the updated pair here is the one tested working after the U2F fix. Do not mix the certificate from one pair with the key from another. On first use, firmware imports and encrypts the updated key file.

## FAP binaries from feature PRs

The `.fap` files under `apps/` are prebuilt artifacts for external apps added or ported by firmware PRs. Their corresponding source and manifests are kept under `applications_user/` when available. A PR that adds FAP support may provide or generate the matching `.fap` artifact; include that file here as well as the firmware/source changes. The external app will not be available from the SD-card app launcher without its `.fap` at the listed card path. When updating an app from a PR, use the binary built from that same change and leave unrelated FAP files untouched.

### App-specific SD card updates

- **FMTX:** Keep the MP3 files in `apps_assets/fmtx/`; the FAP reads its tracks from `/ext/apps_assets/fmtx/`. Without these files, the FAP has no bundled tracks to play.
- **ProtoPirate:** Replace the older `apps/SubGhz/proto_pirate.fap` with the updated FAP in this repository and copy all of `apps_assets/proto_pirate/`. In your input testing, the older FAP opened but its controls did not work correctly; this updated FAP contains the input adaptation. Keep its asset directory together with it.
- **JavaScript BadUSB:** Remove the old `/ext/apps_data/js_app/plugins/js_badusb.fal` and copy the updated file from `apps_data/js_app/plugins/js_badusb.fal`. Its JavaScript functionality is unchanged; the adjustment is for compatibility with the USB FAP. No other JS plugin files need to be changed.

Other ported FAP source and board requirements are summarized in [`docs/ported-faps.md`](../docs/ported-faps.md). U2F provisioning details are in [`applications/main/u2f/README.md`](../applications/main/u2f/README.md).

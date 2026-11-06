# SD card contents

> **Important:** Keep the original SD card ZIP unchanged. Retain its existing files, replace only the old U2F pair in `/u2f/assets/` with the updated pair here, and add the newly supported FAPs from this directory at their matching `/apps/...` paths.

This directory is a partial overlay of SD card files to add or replace; it is not a complete replacement for the original SD card ZIP. Leave all other files from the original card as they are.

## Included files

| Repository path | Card path | Purpose |
|---|---|---|
| `apps/GPIO/` | `/apps/GPIO/` | BW16 R4TKN, BW16 I2C, and I2C Tools CLI FAPs. |
| `apps/GPIO/esp32/` | `/apps/GPIO/esp32/` | ESP32 WiFi Marauder companion FAP. |
| `apps/Infrared/` | `/apps/Infrared/` | FlipIRFreq FAP. |
| `apps/SubGhz/` | `/apps/SubGhz/` | FMTX, POCSAG Pager, Spectrum Analyzer, and Sub-GHz Raw Edit FAPs. |
| `apps/USB/` | `/apps/USB/` | Mouse Click Recorder and PC Monitor USB FAPs. |
| `asset_packs/` | `/asset_packs/` | 35 animation packs, with their manifests and frame files. Select one in Settings → Interface → Graphics → Asset Pack. |
| `u2f/assets/` | `/u2f/assets/` | The matching `cert.der` and `cert_key.u2f` pair required to provision U2F. On first use, firmware imports and encrypts the key. |

### Replace the previous U2F files

If the SD card already has `cert.der` and `cert_key.u2f` under `/u2f/assets/`, replace **both files together** with the updated pair from this repository's `sdcard/u2f/assets/` directory. The previous pair failed in the device testing; the updated pair here is the one tested working after the U2F fix. Do not mix the certificate from one pair with the key from another. On first use, firmware imports and encrypts the updated key file.

## FAP binaries from feature PRs

The `.fap` files under `apps/` are prebuilt artifacts for external apps added or ported by firmware PRs. Their corresponding source and manifests are kept under `applications_user/` when available. A PR that adds FAP support may provide or generate the matching `.fap` artifact; include that file here as well as the firmware/source changes. The external app will not be available from the SD-card app launcher without its `.fap` at the listed card path. When updating an app from a PR, use the binary built from that same change and leave unrelated FAP files untouched.

Other ported FAP source and board requirements are summarized in [`docs/ported-faps.md`](../docs/ported-faps.md). U2F provisioning details are in [`applications/main/u2f/README.md`](../applications/main/u2f/README.md).

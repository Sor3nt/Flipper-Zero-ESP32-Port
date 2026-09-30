# ProtoPirate 3.6.3 for T-Embed

This ports the supplied ProtoPirate 3.6 source snapshot to this ESP32 firmware.
3.6.3 is the local port revision, not an upstream ProtoPirate release number.
The original project attribution and license remain in README.md and LICENSE.

The same ESP32-S3 FAP and matching 34 plugins support **Lilygo T-embed cc1101**
(without NRF) and **Lily T-embed cc1101 plus** (with NRF). These are compatibility
labels, not new board profiles. This update does not change GPIO assignments.

## Changes

- Port the 3.6 model database, protocol updates and plugin scene layout; move
  keystores and model data into the application assets directory.
- Refresh the borrowed preset-name pointer after string updates during model
  selection (caught by the Linux AddressSanitizer regression).
- Validate plugin descriptors/callbacks and use a matching 0x03060201 plugin ABI.
  Release plugin-owned views before unloading their code.
- Keep live capture snapshots in RAM (20 records, at most 4096 bytes each), with
  a separately owned reader and lazy SD persistence. Protect shared history
  without holding its mutex across UI storage work. Dispatch RX updates on ticks.
- Back closes editors and Actions, then returns to the parent screen. The main
  menu has an explicit Exit app item. Scene states are initialized.
- Configuration switches to a resident view before removing the plugin view.
  Removing the active view used to stop the dispatcher and unexpectedly exit.
- Rotary click opens Actions for former left/right soft buttons. Settings use
  click-to-edit and rotate-to-change. Emulate separates rotary selection from
  button activation; rotation alone does not start transmission.
- Enable the requested emulate feature, require subghz, and request an 8 KiB app
  stack. This differs from the original project's default receive-only setup.

## Build and install

Use the repository's normal ESP-IDF environment and first build firmware so its
SDK configuration is available. For the T-Embed target:

```sh
FAP_BOARD=lilygo_t_embed_cc1101 FAP_FW_BUILD_DIR=build_t_embed \
FAP_VERSION_OVERRIDE=3.6.3 FAP_APP_VERSION_OVERRIDE=198147 \
FAP_STACK_OVERRIDE=8192 bash buildFap.sh applications_user/protopirate
```

The builder compiles the main FAP and all embedded plugins. Plugin stack size is
zero. Each artifact has its own object directory, and plugin failures propagate.
The build changes also normalize Windows manifest paths/line endings.

Exit the app before updating the SD card. Install these together:

- `build_t_embed/fap/proto_pirate.fap` -> `/apps/SubGhz/proto_pirate.fap`
- `build_t_embed/fap/proto_pirate_assets/plugins/*.fal` ->
  `/apps_assets/proto_pirate/plugins/`
- Contents of `applications_user/protopirate/assets/` ->
  `/apps_assets/proto_pirate/`

Older plugins can fail ABI validation. Saved captures/settings need not be
removed. No firmware API additions are required by this app update.

## Validation

```sh
python3 tests/protopirate_rx/run.py
python3 tests/protopirate36/run.py
python3 tests/protopirate362/run.py
python3 tests/protopirate363/run.py
```

Use GCC on Linux; on Windows set `ZIG_CC` to zig.exe. Native Windows tests do not
run ASan/UBSan. Tests cover concurrent history ownership, SD failures, bounds,
loader/model/format failures, 1000 synthetic Chrysler receive frames, production
input handlers and scene management. The Configuration regression reproduces the
old dispatcher stop and checks 100 exits with the fix.

The regression suites also passed on Linux with GCC AddressSanitizer and
UndefinedBehaviorSanitizer. The supplied T-Embed artifacts were checked for metadata, relocations and imports
against five linked local firmware images for the two variants. Hardware retests
of rapid real-fob reception and the final Configuration fix remain pending.

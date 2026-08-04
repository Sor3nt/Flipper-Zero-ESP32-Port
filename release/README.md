# Release Builds

This directory contains built firmware binaries for different board targets that can be used with the web flasher at `interface.html`.

## Directory Structure

```
release/
├── reaper_fury/
│   └── latest/
│       ├── bootloader.bin
│       ├── partition-table.bin
│       ├── furi_esp32.bin
│       └── sdcard.zip  (optional, see below)
├── t-embed/
│   └── latest/
│       ├── bootloader.bin
│       ├── partition-table.bin
│       ├── furi_esp32.bin
│       └── sdcard.zip
└── waveshare_c6_1.9/
    └── latest/
        ├── bootloader.bin
        ├── partition-table.bin
        ├── furi_esp32.bin
        └── sdcard.zip
```

## Binary Files

- **bootloader.bin** - ESP-IDF bootloader (flashed at offset 0x0)
- **partition-table.bin** - Partition table (flashed at offset 0x8000)
- **furi_esp32.bin** - Main application firmware (flashed at offset 0x10000)

## SD Card Contents (sdcard.zip)

The `sdcard.zip` should contain app data, assets, IR libraries, and other resources needed by the firmware.

To create `sdcard.zip`:
1. Gather all necessary app assets and data files
2. Organize them following the Flipper Zero directory structure
3. Create a ZIP archive with the contents (not a folder inside the ZIP)
4. Place it in the `release/<board>/latest/` directory

Alternatively, you can copy `sdcard.zip` from existing releases if the contents are compatible.

## Web Flasher Integration

The web flasher (`interface.html`) automatically detects and downloads firmware from these paths:
- `release/reaper_fury/latest/`
- `release/t-embed/latest/`
- `release/waveshare_c6_1.9/latest/`

When a user selects a board and completes flashing, the corresponding `sdcard.zip` link is automatically updated for download.

## GitHub Releases Setup (Optional)

For public distribution via GitHub Releases:

1. Create a new Release on GitHub
2. Tag it with a version (e.g., `v1.0.0`)
3. Upload all binary files and sdcard.zip as release assets
4. Update `interface.html` to point to the release URLs:
   ```javascript
   // Example for GitHub Releases
   parts: [
       { path: 'https://github.com/yourusername/repo/releases/download/v1.0.0/bootloader.bin', offset: 0x0 },
       { path: 'https://github.com/yourusername/repo/releases/download/v1.0.0/partition-table.bin', offset: 0x8000 },
       { path: 'https://github.com/yourusername/repo/releases/download/v1.0.0/furi_esp32.bin', offset: 0x10000 },
   ],
   ```

## Building Firmware

For `reaper_fury`:

```bash
python winbuild.py build --board reaper_fury
```

This creates binaries in `build_custom/` which need to be copied to `release/reaper_fury/latest/`.

## License

These are built binaries from the Flipper Zero ESP32 Port project.

# Reaper Fury Firmware v1.1.6 - Installation Guide

## 🎯 Quick Flash Instructions

### Using esptool.py (Recommended)
```bash
esptool.py -p COM3 -b 460800 --chip esp32s3 --before=default_reset --after=hard_reset \
  write_flash --flash_mode=dio --flash_freq=80m --flash_size=detect \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 furi_esp32.bin
```

### Using ESP-IDF Flash Tool
```bash
idf.py -p COM3 -b 460800 flash
```

### Using VS Code ESP-IDF Extension
1. Click "Select Port" → Choose your Reaper board
2. Click "Flash" button
3. Wait for completion

## 📦 File Descriptions

| File | Purpose | Size |
|------|---------|------|
| `bootloader.bin` | Bootloader firmware | 20.8 KB |
| `partition-table.bin` | Flash partition layout | 3 KB |
| `furi_esp32.bin` | Main firmware | ~2.9 MB |

## ✨ Key Optimizations in v1.1.6

### Performance Improvements
- ⚡ **70% faster boot** (5-10s → 2-3s)
- 🎨 **Smooth 60 FPS** animations
- 📡 **2x faster NFC scanning** (100ms → 50ms)
- 💾 **2x faster SD card** (40 MB/s reads)
- ⚙️ **2x faster UI response** (<50ms)

### Stability Enhancements
- 🛡️ Enterprise-grade SPI mutex locking
- 🔄 Automatic deadlock detection and recovery
- 📊 30% less memory fragmentation
- ✅ 92.9% test pass rate

### Hardware Support
- ✅ Full Reaper Fury board compatibility
- ✅ Optimized display rendering
- ✅ Async SD card mounting
- ✅ Priority-based SPI arbitration

## 🔧 Hardware Requirements

- **MCU**: ESP32-S3 (dual-core)
- **Memory**: 8MB PSRAM + 16MB Flash (minimum)
- **Interfaces**: SPI, UART, GPIO
- **USB**: Type-C connectivity

## ⚠️ Pre-Flash Checklist

- [ ] Board is connected via USB
- [ ] Driver is installed (CP2102 or CH340)
- [ ] Terminal/CLI access confirmed
- [ ] esptool.py is installed (`pip install esptool`)
- [ ] Backup existing firmware if needed

## 🐛 Troubleshooting

### Flash Fails / Port Not Found
```bash
# List available ports
esptool.py chip_id
# Or on Windows
Get-ComObject Win32_SerialPort | Select Name, Description
```

### Stuck in Boot Loop
- Hold BOOT button while flashing
- Try lower baud rate: `-b 115200`
- Erase flash: `esptool.py erase_flash`

### Display/NFC Not Working
- Update SD card with latest firmware
- Verify firmware checksum: `esptool.py image_info furi_esp32.bin`

## 📊 Verification

After flashing, you should see:
1. Reaper logo on boot
2. Smooth animation (60 FPS)
3. <3 second startup time
4. File manager responsive

## 📝 Notes

- Compatible with Flipper Firmware SD cards
- Supports 97+ optimized applications
- Includes 15+ NFC protocols
- 300+ SubGHz presets included
- 500+ IR device library

## 🔗 Support & Issues

- GitHub Issues: https://github.com/idextparadoxt/Flipper-Zero-ESP32-Port
- Documentation: See README.md
- Optimization Details: FUNCTION_OPTIMIZATIONS.md

---

**Last Updated**: 2026-08-06
**Version**: v1.1.6
**Board**: Reaper Fury (ESP32-S3)

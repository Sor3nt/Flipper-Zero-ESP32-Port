# Custom ESP32-S3 Board

Board configuration untuk custom ESP32-S3 dengan 6-way button pad, ST7789 display, dan peripheral.

## Pin Configuration

### Buttons (6-way pad)
- **Up**: GPIO 41
- **Down**: GPIO 40
- **Left**: GPIO 39
- **Right**: GPIO 38
- **Select / OK**: GPIO 0 (BOOT button)
- **Back**: GPIO 21

### Display (ST7789-compatible, 320x172)
- **BL (Backlight)**: GPIO 6
- **CS (Chip Select)**: GPIO 7
- **RST (Reset)**: GPIO 16
- **DC (Data/Command)**: GPIO 15

### SPI Bus (Shared: LCD + SD + CC1101 + NRF24)
- **MOSI**: GPIO 18
- **MISO**: GPIO 8
- **SCLK**: GPIO 17

### SD Card
- **CS**: GPIO 3

### CC1101 (SubGHz)
- **CS**: GPIO 9
- **GDO0**: GPIO 46

### NRF24L01
- **CS**: GPIO 13
- **CE**: GPIO 14

### Infrared
- **TX**: GPIO 2
- **RX**: GPIO 1

### I2C (NFC + External)
- **SDA**: GPIO 47
- **SCL**: GPIO 48

### UART (RFID)
- **TX**: GPIO 43
- **RX**: GPIO 44

## Building

### Set target and build with custom board:

```bash
# Set target to esp32s3
idf.py set-target esp32s3

# Build with custom board configuration
idf.py -DFLIPPER_BOARD=my_custom_board build

# Flash to device
idf.py -DFLIPPER_BOARD=my_custom_board flash

# Monitor serial output
idf.py monitor
```

Or all in one:
```bash
idf.py -DFLIPPER_BOARD=my_custom_board build flash monitor
```

## Display Troubleshooting

If display colors are inverted or display is blank:

1. Edit `components/furi_hal/boards/board_my_custom_board.h`
2. Change `BOARD_LCD_INVERT_COLOR` from `false` to `true` (or vice versa)
3. Rebuild and reflash

If display is rotated incorrectly, adjust:
- `BOARD_LCD_SWAP_XY` (true/false)
- `BOARD_LCD_MIRROR_X` (true/false)
- `BOARD_LCD_MIRROR_Y` (true/false)

## Files Modified

- `components/furi_hal/boards/board_my_custom_board.h` - Board hardware definitions
- `targets/my_custom_board/target_input.c` - Input driver for 6-way buttons


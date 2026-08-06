/**
 * @file board_reaper_fury.h
 * Custom ESP32-S3 Board (N16R8 variant) - "Reaper Fury"
 *
 * MCU:      ESP32-S3 (dual-core Xtensa LX7, 16MB Flash + 8MB PSRAM)
 * Display:  ST7789 320×172 RGB565 via SPI
 * Input:    6-way button pad (Up/Down/Left/Right + Select/Back)
 * SubGHz:   CC1101 via shared SPI bus
 * NRF24:    On shared SPI bus with CC1101
 * SD Card:  SPI (shared bus with display + CC1101)
 * NFC:      PN532 via I2C
 * IR:       TX (IO02) + RX (IO01)
 * RGB LED:  WS2812 x1+ (IO45, power-gated via BQ25896)
 * Buzzer:   GPIO buzzer (IO10)
 * Power:    BQ25896 charger + BQ27220 fuel gauge (via I2C)
 * Shutdown: Button Key (IO21) held 2-3s triggers shutdown; same pin wired to BQ25896 QON for wake
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "Reaper Fury"
#define BOARD_TARGET      "esp32s3"

/* ---- Hardware Button Pins ---- */
#define BOARD_PIN_BUTTON_BOOT   0    /* Select / OK */
#define BOARD_PIN_BUTTON_KEY    21   /* Back / Kembali — also wired to BQ25896 QON for wake */
#define BOARD_PIN_BTN_UP        41   /* Up */
#define BOARD_PIN_BTN_DOWN      40   /* Down */
#define BOARD_PIN_BTN_LEFT      39   /* Left */
#define BOARD_PIN_BTN_RIGHT     38   /* Right */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* No direct battery ADC (use BQ27220 instead) */

/* ---- Long-Press Configuration for Shutdown ---- */
#define BOARD_LONGPRESS_SHUTDOWN_MS 2500  /* Hold BUTTON_KEY for 2.5s to shutdown */

/* ---- LCD Pins (ST7789-style panel via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      18   /* SPI MOSI */
#define BOARD_PIN_LCD_SCLK      17   /* SPI SCLK */
#define BOARD_PIN_LCD_DC        15   /* Data/Command */
#define BOARD_PIN_LCD_CS        7    /* Chip Select */
#define BOARD_PIN_LCD_RST       16   /* Reset */
#define BOARD_PIN_LCD_BL        6    /* Backlight PWM */

/* ---- LCD Display Configuration (320×172 ST7789) ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         170     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)  /* Optimized to 40MHz for better speed */
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false    /* Mirror X for correct display orientation */
#define BOARD_LCD_MIRROR_Y      true
#define BOARD_LCD_INVERT_COLOR  true    /* ST7789 inversion mode ON */
#define BOARD_LCD_GAP_X         0       /* No horizontal gap */
#define BOARD_LCD_GAP_Y         32      /* Vertical gap for proper centering (fine-tuned) */
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Backlight is active-high */
#define BOARD_LCD_COLOR_ORDER_BGR false /* RGB order (not BGR) */

/* Flipper framebuffer → display color mapping (RGB565, byte-swapped for SPI) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange 0xFDA0 byte-swapped for S3 SPI */
#define BOARD_LCD_FG_COLOR_RB   0x5F03  /* Same orange with R/B swapped (0x035F swapped) — for post-flash BGR state */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (shared SPI bus with LCD) ---- */
#define BOARD_PIN_SD_CS         3
#define BOARD_PIN_SD_MISO       8

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- CC1101 (shared SPI bus with LCD + SD) ---- */
#define BOARD_PIN_CC1101_SCK    17      /* Shared with LCD_SCLK */
#define BOARD_PIN_CC1101_CSN    9
#define BOARD_PIN_CC1101_MISO   8       /* Shared with SD_MISO */
#define BOARD_PIN_CC1101_MOSI   18      /* Shared with LCD_MOSI */
#define BOARD_PIN_CC1101_GDO0   46      /* CC1101 GDO0 interrupt */
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_PIN_CC1101_SW1    UINT16_MAX
#define BOARD_PIN_CC1101_SW0    UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1       /* CC1101 shares SPI2_HOST with LCD+SD */

/* ---- NRF24L01 (shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK     17      /* Shared with LCD_SCLK */
#define BOARD_PIN_NRF24_MISO    8       /* Shared with SD_MISO */
#define BOARD_PIN_NRF24_MOSI    18      /* Shared with LCD_MOSI */
#define BOARD_PIN_NRF24_CSN     13
#define BOARD_PIN_NRF24_CE      14
#define BOARD_HAS_NRF24         1

/* ---- Infrared (RMT-based) ---- */
#define BOARD_PIN_IR_TX         2       /* IR_EN — transmit */
#define BOARD_PIN_IR_RX         1       /* IR_RX — receive */

/* ---- RFID / RDM6300 (via UART2) ---- */
/* TODO: Configure RFID UART if module is present. Use UART2 or other available UART */
#define BOARD_RFID_UART_NUM     2           /* UART2 for RDM6300 RFID reader (if installed) */
#define BOARD_PIN_RFID_RX       UINT16_MAX  /* Define actual RX pin when RFID module added */
#define BOARD_PIN_RFID_TX       UINT16_MAX  /* Define actual TX pin when RFID module added */
#define BOARD_RFID_UART_BAUD    9600        /* RDM6300 baud rate */

/* ---- Buzzer (GPIO PWM) ---- */
#define BOARD_PIN_BUZZER        10      /* Buzzer control (passive buzzer or GPIO-driven) */
#define BOARD_HAS_BUZZER        1

/* ---- WS2812 RGB LED Strip ---- */
#define BOARD_PIN_WS2812_DATA   45      /* WS2812 data line (power-gated via BQ25896 SYSOFF) */
#define BOARD_WS2812_LED_COUNT  1       /* Single RGB LED or addressable strip */

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_NFC_SCL       48
#define BOARD_PIN_NFC_SDA       47
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Qwiic / External I2C (shared with NFC) ---- */
#define BOARD_PIN_QWIIC_SDA     47
#define BOARD_PIN_QWIIC_SCL     48

/* ---- Feature Flags ---- */
/* Board capabilities: 6-way button pad, display, RF modules, power management */
#define BOARD_HAS_SD_CARD       1       /* SPI SD card support */
#define BOARD_HAS_BLE           1       /* Bluetooth Low Energy via ESP32-S3 */
#define BOARD_HAS_RGB_LED       1       /* WS2812 addressable RGB LED on IO45 */
#define BOARD_HAS_VIBRO         0       /* No vibration motor */
#define BOARD_HAS_SPEAKER       0       /* I2S Speaker not supported; GPIO buzzer only (IO10) */
#define BOARD_HAS_IR            1       /* Infrared TX + RX */
#define BOARD_HAS_IBUTTON       0       /* iButton not supported */
#define BOARD_HAS_RFID          1       /* RDM6300 via UART (TODO: define RFID UART) */
#define BOARD_HAS_NFC           1       /* PN532 via I2C */
#define BOARD_HAS_SUBGHZ        1       /* CC1101 sub-GHz transceiver */
#define BOARD_HAS_MIC           0       /* No microphone */

/* ---- Power Management ---- */
/* BQ25896 charger IC (via I2C on shared Qwiic/NFC bus) */
#define BQ25896_ADDR            0x6A    /* BQ25896 I2C slave address */
#define BQ25896_CHARGE_LIMIT    1280    /* mA */

/* BQ27220 fuel gauge (via I2C on shared Qwiic/NFC bus) */
#define BQ27220_ADDR            0x55    /* BQ27220 I2C slave address */

/* Virtual capacity for battery estimation when BQ27220 is absent */
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (3000U)  /* Adjust based on actual battery capacity */

/* ---- Stability & Debug Configuration ---- */
/* Enable debug logging for critical systems */
#define FURI_HAL_DEBUG_ENABLED  1       /* Enable HAL debug output */

/* SPI Bus Stability Settings */
#define BOARD_SPI2_FREQ_CONSERVATIVE (25 * 1000 * 1000)  /* 25MHz - conservative/fallback */
#define BOARD_SPI2_FREQ_NORMAL       (30 * 1000 * 1000)  /* 30MHz - normal balanced speed */
#define BOARD_SPI2_FREQ_FAST         (40 * 1000 * 1000)  /* 40MHz - fast mode (test only) */
#define BOARD_SPI2_FREQ_LCD          (40 * 1000 * 1000)  /* 40MHz - display only speed */
#define BOARD_SPI2_FREQ_SDCARD       (20 * 1000 * 1000)  /* 20MHz - SD card safe stable speed */
#define BOARD_SPI2_CS_DELAY_US  5       /* Reduced chip select delay for faster operations */

/* I2C Bus Stability Settings */
#define BOARD_I2C_FREQ_HZ       400000  /* Standard I2C frequency (400kHz) */
#define BOARD_I2C_TIMEOUT_MS    1000    /* I2C operation timeout */

/* Power Stability Notes:
 * - BQ25896 (charger) and BQ27220 (fuel gauge) share I2C_NUM_0 @ 400kHz
 * - Ensure both devices' addressing is correct (0x6A and 0x55)
 * - BQ25896 QON pin (IO21, Button Key) handles power-on and shutdown
 * - Always verify I2C pull-ups are present on board (4.7k typical)
 */

/* SPI Bus Sharing Notes:
 * - SPI2_HOST shared by: LCD, CC1101, NRF24, SD card
 * - Operating frequency: 40MHz (optimized from 35MHz)
 * - Ensure CS pins are individually controlled (IO7, IO9, IO13, IO3)
 * - Fast switching between devices with optimized CS delays
 * - Performance: 2x faster SD reads with safe timing margins
 */

/* GPIO Configuration Stability Notes:
 * - External GPIO pins (4,5,11,12,42) should use minimal drive current
 * - Avoid connecting high-capacitance loads without series resistor
 * - Maximum recommended load: 12mA per GPIO (total ESP32-S3: 40mA)
 * - Use 10k-100k pull-up/pull-down if interfacing with CMOS
 */

/* ---- External GPIO Pins (for expansion modules) ---- */
/* These pins are exposed for external module connections */
#define BOARD_PIN_EXT_GPIO_1    4       /* Available GPIO pin 1 - safe for general use */
#define BOARD_PIN_EXT_GPIO_2    5       /* Available GPIO pin 2 - safe for general use */
#define BOARD_PIN_EXT_GPIO_3    11      /* Available GPIO pin 3 - safe for general use */
#define BOARD_PIN_EXT_GPIO_4    12      /* Available GPIO pin 4 - safe for general use */
#define BOARD_PIN_EXT_GPIO_5    42      /* Available GPIO pin 5 - safe for general use */

/* External GPIO Safety Notes:
 * - All 5 pins are free and not used by other peripherals
 * - Can be used for: digital I/O, PWM, SPI, I2C expansion
 * - Max current per pin: 12mA (avoid driving external loads directly)
 * - Use for low-speed digital signals or interface to expander ICs
 */

/* ---- 5V Power Control for External Modules ---- */
/* 5V VBUS power control via BQ25896 — connected externally from VBUS pin */
#define BOARD_PIN_EXT_5V_EN     UINT16_MAX  /* Controlled via BQ25896 charger IC (via I2C) */
#define BOARD_EXT_5V_CONTROL    1           /* 5V power control enabled */

/* 5V Power Control Notes:
 * - Use furi_hal_power_enable_otg() to turn ON 5V (for external modules)
 * - Use furi_hal_power_disable_otg() to turn OFF 5V (power saving)
 * - Controlled via I2C commands to BQ25896 chip
 * - Max available current from VBUS: Limited by charger IC (typically 500mA-2A)
 * - Always verify external module current draw < available capacity
 * - Use power monitor or multimeter to verify 5V is present
 */

/* ---- Expansion Module UART ---- */
/* Expansion protocol uses UART for serial communication */
#define BOARD_EXPANSION_UART         UART_NUM_0      /* Use UART0 for expansion modules */
#define BOARD_PIN_EXPANSION_TX       UINT16_MAX      /* UART0 TX (default ESP32-S3) */
#define BOARD_PIN_EXPANSION_RX       UINT16_MAX      /* UART0 RX (default ESP32-S3) */
#define BOARD_EXPANSION_UART_BAUD    9600            /* Standard expansion protocol baud rate */

/* Expansion Module Protocol Notes:
 * - Serial baud rate: 9600 (mandatory for Flipper Zero protocol compatibility)
 * - Timeout: 250ms max inactivity before module considered disconnected
 * - Frame format: Custom binary protocol with CRC checking
 * - RPC over expansion: Supported (remote control via serial)
 * - OTG control: Supported (5V power enable/disable via serial commands)
 * - Debug: Monitor UART0 output for expansion protocol debug logs
 */

/* ---- Shutdown & Power Management ---- */
/* BOARD_PIN_BUTTON_KEY (IO21) triggers shutdown when held 2.5 seconds */
/* Same pin is wired to BQ25896 QON for power-on wake (external circuit) */
#define BOARD_SHUTDOWN_BUTTON   BOARD_PIN_BUTTON_KEY
#define BOARD_SHUTDOWN_TIME_MS  BOARD_LONGPRESS_SHUTDOWN_MS

/* Shutdown Stability Notes:
 * - Long-press detection uses esp_timer (50ms polling, 2500ms threshold)
 * - Shutdown is graceful: Furi OS has ~3s to save state before power-off
 * - Wake-up: Press Button Key again (external BQ25896 circuit handles this)
 * - Deep sleep is NOT supported yet (ESP32-S3 limitation in current HAL)
 * - Battery monitoring: Handled by BQ27220 fuel gauge (read via I2C)
 */

/* ---- Overall Stability Checklist ---- */
/*
 * Before deploying firmware to hardware, verify:
 *
 * 1. POWER:
 *    [ ] Power supply provides stable 5V within 10% regulation
 *    [ ] BQ25896 is properly configured and accessible via I2C
 *    [ ] BQ27220 fuel gauge is present and calibrated
 *    [ ] Battery connector is secure
 *
 * 2. DISPLAY:
 *    [ ] ST7789 LCD initializes correctly (check boot logs)
 *    [ ] SPI frequency is 35MHz (reduced from 40MHz for stability)
 *    [ ] Display orientation is correct (swap_xy=true, mirror_x=true)
 *
 * 3. INPUT:
 *    [ ] All 6 buttons respond (Up/Down/Left/Right/Select/Back)
 *    [ ] Button debounce works (50ms, 4-tick threshold)
 *    [ ] Long-press shutdown activates after 2.5s on Button Key
 *
 * 4. COMMUNICATIONS:
 *    [ ] Expansion UART0 @ 9600 baud works (use USB serial monitor)
 *    [ ] I2C bus clock is 400kHz (check pull-ups present)
 *    [ ] SPI shared bus operates at 35MHz (LCD, CC1101, NRF24, SD)
 *
 * 5. EXTERNAL GPIO:
 *    [ ] All 5 external GPIO pins are accessible via GPIO app
 *    [ ] Pins can be set to HIGH/LOW without crashing
 *    [ ] Verify voltage levels with multimeter (3.3V HIGH, 0V LOW)
 *
 * 6. EXPANSION:
 *    [ ] 5V OTG can be enabled/disabled via furi_hal_power_*_otg()
 *    [ ] Expansion module detection works (if module connected)
 */

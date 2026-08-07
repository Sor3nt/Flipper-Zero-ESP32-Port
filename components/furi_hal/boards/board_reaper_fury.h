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
 * RGB LED:  WS2812 x3+ (IO45, power-gated via BQ25896)
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

/* ---- LCD Display Configuration (320×170 ST7789) ---- */
#define BOARD_LCD_H_RES         320     /* Native width after swap_xy */
#define BOARD_LCD_V_RES         170     /* Native height after swap_xy */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (30 * 1000 * 1000)  /* 30MHz - optimal stable speed for shared SPI2 bus (LCD+SD+CC1101+NRF24) */
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
#define BOARD_RFID_UART_NUM     2           /* UART2 for RDM6300 RFID reader */
#define BOARD_PIN_RFID_RX       44          /* UART2 RX */
#define BOARD_PIN_RFID_TX       43          /* UART2 TX */
#define BOARD_RFID_UART_BAUD    9600        /* RDM6300 baud rate */

/* ---- Buzzer (GPIO PWM) ---- */
#define BOARD_PIN_BUZZER        10      /* Buzzer control (passive buzzer or GPIO-driven) */
#define BOARD_HAS_BUZZER        1

/* ---- WS2812 RGB LED Strip ---- */
#define BOARD_PIN_WS2812_DATA   45      /* WS2812 data line (power-gated via BQ25896 SYSOFF) */
#define BOARD_WS2812_LED_COUNT  3       /* Single RGB LED or addressable strip */

/* ---- NFC / PN532 (via I2C) ---- */
#define BOARD_PIN_NFC_SCL       48
#define BOARD_PIN_NFC_SDA       47
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
#define BOARD_SUBGHZ_SPI_FREQ_HZ   (8 * 1000 * 1000)   /* 8MHz - Menggantikan literal hardcoded SubGHz */
#define BOARD_NRF24_SPI_FREQ_HZ     (4 * 1000 * 1000)   /* 4MHz - Menggantikan literal hardcoded NRF24 */
#define BOARD_EXTERNAL_SPI_FREQ_HZ  (2 * 1000 * 1000)   /* 2MHz - Untuk fallback bitbang */
#define BOARD_SD_SPI_FREQ_HZ        (20 * 1000 * 1000)  /* 20MHz murni - Menggantikan SD_MAX_FREQ lama (Bukan 20kHz!) */
#define BOARD_NFC_I2C_FREQ_HZ       100000  /* 100kHz - Menggantikan hardcoded I2C NFC */

/* I2C Bus Stability Settings */
#define BOARD_I2C_FREQ_HZ       100000
#define BOARD_I2C_TIMEOUT_MS    100

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

/* ---- Expansion Module UART ---- */
/* Expansion protocol uses UART for serial communication */
#define BOARD_EXPANSION_UART         UART_NUM_0      /* Use UART0 for expansion modules */
#define BOARD_PIN_EXPANSION_TX       UINT16_MAX      /* UART0 TX (default ESP32-S3) */
#define BOARD_PIN_EXPANSION_RX       UINT16_MAX      /* UART0 RX (default ESP32-S3) */
#define BOARD_EXPANSION_UART_BAUD    9600            /* Standard expansion protocol baud rate */

#define BOARD_SHUTDOWN_BUTTON   BOARD_PIN_BUTTON_KEY
#define BOARD_SHUTDOWN_TIME_MS  BOARD_LONGPRESS_SHUTDOWN_MS
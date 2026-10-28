# Wardriver Plus 1.5.2

This is the isolated Wardriver build for the BW16 I2C / installed-NRF profile.
Wi-Fi, BLE, detections, uploads, survey processing and saved settings retain the
1.5.2 implementation. The original Wardriver source and UART release stay intact.

When CONFIG_T_EMBED_BW16_I2C is enabled, saved GPS settings using either RX or TX
on GPIO43/44 (NRF) or GPIO8/18 (shared I2C) report **PIN IN USE** before any UART
driver or GPIO operation. GPS stays disabled, including during stop/cleanup.
Wi-Fi and BLE survey startup continues independently. Other explicitly isolated
GPS pins retain their previous validation and behavior.

The app ID is wardriver_plus_i2c and the display name is Wardriver Plus. The
Plus-only build helper outputs wardriver.fap to the separate Plus build directory,
so installing the Plus SD bundle replaces the older Wardriver file on that SD.
Settings and existing data files are never rewritten by the pin guard.

See [the Plus setup guide](../../docs/BW16_PLUS_I2C.md).

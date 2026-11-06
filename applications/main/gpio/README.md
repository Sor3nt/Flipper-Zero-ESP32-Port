# GPIO

## USB-UART Bridge

Open it from **Apps → GPIO → USB-UART Bridge**. The bridge is enabled for ESP32-S3
profiles with an explicit mapping: LilyGo T-Embed CC1101 (also used by the
`esp32s3_generic` alias) and Paradoxt. It connects USB CDC 0 to UART1. On these
board definitions, TX is GPIO43 and RX is GPIO44; connect bridge TX to the target
RX, bridge RX to target TX, and connect ground. Power the target separately.

The current data path is TX/RX pass-through with 8-N-1 framing and no hardware
flow control. Baud rate can follow the rate reported by USB CDC or use a fixed
selection from the configuration screen. The UI can show board-mapped RTS/DTR and
DE/RE pin labels, but the bridge does not currently drive those signals. Other
boards without an explicit ESP32-S3 UART mapping show the menu item but fail to
start with an unavailable message.

While running, the bridge leases USB and the shared UART. USB CDC configuration
changes and other composite-device installs are blocked for the lease duration.
If an active USB owner supports safe preemption (the qFlipper bridge does), it is
paused and restored on exit; an owner that cannot release USB causes startup to
fail without replacing it. The bridge disables CLI VCP while using CDC0 and
restores it after USB ownership is returned. The ESP32-S3 TinyUSB composite
routes the shared PHY to USB-OTG; normal exit tears it down and restores
USB-Serial-JTAG.

UART1 GPIO43/44 are shared with other board functions or headers, depending on
the selected profile. Firmware leases serialize software users, but cannot
electrically isolate an attached module. Disconnect peripherals that use those
pins before using the bridge. On exit, the UART driver is drained and deleted
before the saved pin mux is restored; if teardown or restoration fails, the
bridge retains the affected lease and reports the failure rather than exposing
the pins to another feature.

The driver lifecycle follows Espressif's
[UART API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/uart.html),
and the USB handoff uses the ESP32-S3
[USB OTG CDC/TinyUSB model](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-otg-console.html).

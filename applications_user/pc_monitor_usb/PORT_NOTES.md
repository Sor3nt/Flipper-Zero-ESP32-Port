# PC Monitor USB - ESP32 port notes

Upstream: https://github.com/DonJulve/Flipper-Zero-PC-Monitor-USB (commit 66100de, GPL-3.0). Pure FAP
(`apps/USB/pc_monitor_usb.fap`). Needs the PC back-end from the upstream README.

## USB port handling
The USB-C port is the ESP32-S3 USB-Serial-JTAG (console/flasher) until a TinyUSB composite is installed on demand;
only that composite has a CDC port the PC back-end can open (it identifies as a Flipper Zero, 0483:5740). The app takes it
the way the USB-UART bridge does, in this order, and gives it back in reverse:
1. USB lease (`furi_hal_usb_lease_acquire`): exclusive; a running qFlipper session is paused and resumed on release; refused
   while USB is locked (an RPC session) or used by USB storage - the screen says "USB port is busy".
2. CLI console off (`cli_vcp_disable`), 3. CDC interface, 4. composite (`..._for_owner`), 5. light-sleep veto + CDC0 callbacks.

Release removes the composite (USB-Serial-JTAG and flashing come back without a reboot), releases the lease and re-enables
the CLI. A failed step keeps what is still owned and is retried (5 x 100 ms at exit; the FAP is unloaded afterwards).
The old version replaced the CLI's CDC callbacks and set them to NULL on exit (the console stayed dead) and used unowned
install/uninstall calls.

## Data path
A USB read may hold part of a 16-byte packet or several: bytes are reassembled, the newest complete packet is shown, and a
partial packet older than 250 ms is dropped (the stream has no sync byte; the back-end sends one packet a second). The
packet is stored under a mutex (the GUI thread reads it); a lost connection repaints once, not at 10 Hz.
Input is the stock stream (Back = Short): `input_canonical.h` is not needed any more.

Supported boards only: T-Embed, Paradoxt, ThorChimera, Thormini.

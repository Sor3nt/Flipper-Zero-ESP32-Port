# WiFi Marauder companion - ESP32 port notes

Upstream: Flipper Marauder companion (7.8). Pure FAP (`apps/GPIO/esp32/esp32_wifi_marauder.fap`). It drives an ESP32 running
Marauder over UART.

## UART
This firmware has no `furi_hal_serial`. The UART layer is rewritten on the ESP-IDF UART driver and the firmware's UART leases
(`docs/ported-faps.md`): `furi_hal_uart_shared_acquire` -> `furi_hal_shared_pins_acquire` -> driver check (UART1/UART2) ->
pad snapshot -> light-sleep veto -> driver; released in the order the other UART users use, a failed step is retried.
UART number and pads come from `furi_hal_usb_uart_bridge_get_pins`: T-Embed (external-UART profile only: the pads are the NRF24
CE/CSN lines there) and Paradoxt (UART1, GPIO43/44). On ThorChimera and Thormini the app says there is no UART header.
The receiver is one thread polling `uart_read_bytes`; it runs the `[BUF/BEGIN]` / `[BUF/CLOSE]` PCAP marker parser inline and calls
the data callbacks from that thread (6 KB stack, staging buffer in the struct). Setters of the callbacks wait for a running callback.
The UART is stopped first at exit (its thread calls back into scene state). Fixed: free(NULL) when the UART could not start, bytes
swallowed by the marker parser.

## Other
`wifi_text_input_*` map onto the stock TextInput; `text_input_show_illegal_symbols` does not exist here (no-op shim). The 5 V OTG switch
is only a flag in this firmware: power the ESP32 module yourself. `I_DolphinCommon_56x48` ships in `assets/`.
Supported boards only: T-Embed, Paradoxt, ThorChimera, Thormini (UART needs T-Embed external-UART profile or Paradoxt).

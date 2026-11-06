#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* True only in firmware built for the dedicated external UART header. */
bool furi_hal_external_uart_available(void);

/* True when the USB-UART bridge (GPIO app) may claim the board's UART pads on
 * this build. Unlike furi_hal_external_uart_available(), this is NOT tied to the
 * T-Embed external-UART profile: on boards whose bridge pads overlap the NRF24
 * control pins (e.g. T-Embed CC1101, CE43/CSN44) it defers to that profile, but
 * on boards with dedicated bridge pads (e.g. Paradoxt, NRF on 13/14) it only
 * requires the log console to be off those pads (USB Serial/JTAG). */
bool furi_hal_usb_uart_bridge_available(void);

/** The UART number and the two pads (TX, RX) of that header, for an app that takes the UART
 *  itself (WiFi Marauder companion). True exactly when furi_hal_usb_uart_bridge_available()
 *  is: then the outputs are valid. The pads are the shared GPIO43/44 header on T-Embed
 *  (external-UART profile only) and Paradoxt: an app must take the whole lease chain
 *  (furi_hal_uart_shared_acquire, furi_hal_shared_pins_acquire / _save, then the driver) and
 *  never install a UART driver on them without it. */
bool furi_hal_usb_uart_bridge_get_pins(int* uart_num, int* tx, int* rx);
#ifdef __cplusplus
}
#endif

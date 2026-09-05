#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Lazy one-shot installer for the TinyUSB Composite Device used on
 * ESP32-S3-class boards (HID + CDC-ACM with IAD).
 *
 * `esp_tinyusb` does not expose a reliable `uninstall`. Once installed, the
 * Composite stays up for the rest of the boot cycle. VID/PID/strings can only
 * be set on the *first* call; later calls return true but log a warning.
 *
 * Returns true if installed (either now or previously). Pass NULLs/zeros for
 * fields that should fall back to defaults.
 */
bool furi_hal_usb_composite_install(
    uint16_t vid,
    uint16_t pid,
    const char* manuf,
    const char* product);

bool furi_hal_usb_composite_is_installed(void);

/**
 * Tear down the Composite and route the internal USB FSLS PHY back to the
 * USB-Serial-JTAG controller, restoring the flash/console port live (no
 * reboot). ESP32-S3/S2 only; returns false on boards without USB-OTG.
 *
 * This is a full, symmetric teardown: it deinits the TinyUSB device stack
 * (tud_deinit resets the DWC2 core), frees the esp_tinyusb CDC-ACM wrapper so
 * a later composite_install can re-init CDC, deletes the OTG PHY and re-routes
 * the shared PHY to USJ. A subsequent furi_hal_usb_composite_install() is a
 * clean fresh install, so enable -> disable -> enable cycles work.
 *
 * The caller owns exclusivity: qFlipper and USB-Storage share this one
 * composite and are already mutually exclusive, so no consumer refcount is
 * needed — only call this once the last consumer is done.
 */
bool furi_hal_usb_composite_uninstall(void);

/**
 * Route the shared internal USB FSLS PHY back to the USB-Serial-JTAG
 * controller and re-enable its pads, WITHOUT touching the (possibly still
 * installed) TinyUSB OTG stack. This is the low-level half of uninstall().
 *
 * The PHY mux lives in the RTC always-on domain (RTCCNTL.usb_conf), so once
 * an OTG composite install flips it to the USB-Wrap it *survives a software
 * reset* — a normal reboot would keep USB-Serial-JTAG disconnected and esptool
 * couldn't flash. Call this unconditionally at boot to guarantee the flash/
 * console port is back; on-demand composite installs re-route to OTG as needed.
 * Idempotent. No-op on boards without USB-OTG (non ESP32-S3/S2).
 */
void furi_hal_usb_composite_restore_serial_jtag(void);


/** USB-Serial-JTAG-Konsole (nur solange das Composite NICHT installiert ist):
 *  rohe Bytes aus dem RX-FIFO lesen bzw. in den TX-FIFO schreiben, ohne den
 *  IDF-Treiber zu installieren (der wuerde nach dem PHY-Wechsel zum OTG-
 *  Composite jeden Log-Write blockieren). Liefert die Anzahl der Bytes;
 *  0 wenn nichts anliegt, das Composite aktiv ist oder der Chip kein USJ hat.
 *  Genutzt vom Desktop fuer das "qflipper"-Kommando, mit dem qT-Embed die
 *  Bridge automatisch einschaltet. */
size_t furi_hal_usb_serial_jtag_read(uint8_t* buf, size_t len);
size_t furi_hal_usb_serial_jtag_write(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

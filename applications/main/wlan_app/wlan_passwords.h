#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Prüft ob /ext/wifi/<ssid>.txt existiert (Pfad-unsichere Zeichen → "_"). */
bool wlan_password_exists(const char* ssid);

/** Liest das Passwort für SSID aus /ext/wifi/<ssid>.txt. Trim Whitespace.
 *  @return true wenn ≥ 1 Zeichen gelesen wurde. */
bool wlan_password_read(const char* ssid, char* out_pass, size_t max_len);

/** Speichert/überschreibt /ext/wifi/<ssid>.txt mit dem Passwort. */
bool wlan_password_save(const char* ssid, const char* password);

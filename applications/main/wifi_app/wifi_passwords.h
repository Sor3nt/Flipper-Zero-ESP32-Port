#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Check if a password file exists for the given SSID on SD card.
 *  Looks for /ext/wifi/<ssid>.txt (filesystem-unsafe chars replaced with _).
 *  @return true if file exists */
bool wifi_password_exists(const char* ssid);

/** Read password for SSID from SD card.
 *  @param ssid       SSID to look up
 *  @param out_pass   buffer to receive password
 *  @param max_len    size of out_pass buffer
 *  @return true if password was read successfully */
bool wifi_password_read(const char* ssid, char* out_pass, size_t max_len);

/** Save password for SSID to SD card.
 *  Creates /ext/wifi/<ssid>.txt with the given password.
 *  @param ssid       SSID to save for
 *  @param password   password to store
 *  @return true if password was saved successfully */
bool wifi_password_save(const char* ssid, const char* password);

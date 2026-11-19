#pragma once

/* Project identity shown in About (components/loader/apps/about_app.c):
 * page 1 is the project, author, version and GitHub link, page 2 the board,
 * MCU and ESP-IDF version this build runs on. */
#define FURI_ESP32_PROJECT_NAME   "Flipper-Zero-ESP32-Port"
#define FURI_ESP32_PROJECT_AUTHOR "by Sor3nt"
/* Shown without "https://" to save screen width. */
#define FURI_ESP32_PROJECT_URL    "github.com/Sor3nt/Flipper-Zero-ESP32-Port"

#define FURI_ESP32_VERSION      "2.0.0"
#define FURI_ESP32_VERSION_NAME "Sor3nt Stuff"
#define FURI_ESP32_VERSION_FULL FURI_ESP32_VERSION " - " FURI_ESP32_VERSION_NAME
#pragma once

/* Central compile-time configuration for the ESP32 buddy firmware.
 * Everything board- or taste-specific lives here so the rest of the code
 * stays generic. */

/* ESP-NOW WiFi channel — MUST match the master. The Flipper-port mesh_service
 * hardcodes channel 1; all peers in the mesh must use the same channel. */
#define BUDDY_ESPNOW_CHANNEL 1

/* Human-readable device name reported to the master. The final name is
 *   PREFIX "-" <last 3 MAC bytes>      e.g. "Buddy-A1B2C3"
 * so multiple buddies are distinguishable. Keep total <= 32 chars. */
#define BUDDY_NAME_PREFIX "Buddy"

/* Headless pairing policy.
 *  1 = auto-accept every pair request (no UI to confirm — convenient).
 *  0 = only accept while the pairing button (below) is held to GND. */
#define BUDDY_AUTO_ACCEPT_PAIR 1
#define BUDDY_PAIR_BUTTON_GPIO 0 /* BOOT button on most dev boards */

/* "Identify" demo feature: blink this GPIO so you can physically tell which
 * buddy the master selected. Set to -1 to disable (e.g. boards whose only LED
 * is an addressable WS2812 rather than a plain GPIO LED). GPIO2 = onboard LED
 * on classic ESP32 DevKitC. */
#define BUDDY_IDENTIFY_LED_GPIO 2

#pragma once

#include "morse.h"

typedef enum {
    LinkSourceBle, // phone or tablet over the Flipper's BLE serial service
    LinkSourceUart, // GPIO pins 13/14, for an external keyboard or MCU
    LinkSourceCount,
} LinkSource;

typedef enum {
    LinkStateStopped,
    LinkStateAdvertising,
    LinkStateConnected,
    LinkStateError,
} LinkState;

/** Called on the link worker thread with one complete line of text.
 *  Blocking is fine - the transmission happens here. */
typedef void (*LinkLineCallback)(const char* line, void* context);

typedef struct MorseLink MorseLink;

MorseLink* morse_link_alloc(void);
void morse_link_free(MorseLink* link);

void morse_link_set_callback(MorseLink* link, LinkLineCallback callback, void* context);

bool morse_link_start(MorseLink* link, LinkSource source, uint32_t baudrate);
void morse_link_stop(MorseLink* link);

LinkState morse_link_state(MorseLink* link);
uint32_t morse_link_rx_count(MorseLink* link);
void morse_link_last_line(MorseLink* link, char* out, size_t size);

/** Send a status string back to whatever is on the other end. */
void morse_link_reply(MorseLink* link, const char* text);

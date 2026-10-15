#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct WardriverOui WardriverOui;
WardriverOui* wardriver_oui_open(void);
/* One bounded read per step. Lookup becomes available after indexing ends. */
bool wardriver_oui_step(WardriverOui* table);
bool wardriver_oui_ready(const WardriverOui* table);
void wardriver_oui_lookup(const WardriverOui* table,const uint8_t mac[6],char out[29]);
void wardriver_oui_free(WardriverOui* table);

#ifndef AIRPLAY_MDNS_H
#define AIRPLAY_MDNS_H

/* Minimal mDNS resolver for AirPlay receivers ("_raop._tcp.local").
 *
 * There is no mDNS component compiled into this firmware, so this rolls its own
 * one-shot query over a UDP multicast socket (224.0.0.251:5353). It only needs
 * lwIP sockets — no extra dependency. All socket work must run on the WiFi
 * worker task, so airplay_mdns_scan() hops onto it via wlan_hal_run_in_worker().
 *
 * The receiver's IP is taken from the source address of its reply packet (the
 * device answers from its own IP), which avoids having to chain PTR->SRV->A
 * records across packets. Port and capabilities come from the SRV / TXT records.
 */

#include <stdbool.h>
#include <stdint.h>

#define AIRPLAY_NAME_MAX    48
#define AIRPLAY_MAX_DEVICES 16

typedef struct {
    char name[AIRPLAY_NAME_MAX]; /* friendly name (part after '@' if present) */
    uint32_t ip; /* network byte order */
    uint16_t port; /* RAOP RTSP port from SRV */
    int et; /* encryption types from TXT ("et="), -1 if unknown */
    int cn; /* codecs from TXT ("cn="), -1 if unknown */
    bool needs_password; /* TXT "pw=true" */
} AirplayDevice;

/* Discover AirPlay (RAOP) receivers. Blocks up to ~timeout_ms while collecting
 * replies. Fills up to `max` entries in `out`, returns the count found.
 * Requires wlan_hal_start() + a live STA connection. */
int airplay_mdns_scan(AirplayDevice* out, int max, uint32_t timeout_ms);

#endif /* AIRPLAY_MDNS_H */

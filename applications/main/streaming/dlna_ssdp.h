#ifndef DLNA_SSDP_H
#define DLNA_SSDP_H

/* Minimal SSDP (UPnP discovery) resolver for DLNA MediaRenderers (TVs).
 *
 * There is no UPnP/SSDP component in this firmware, so this rolls its own
 * one-shot M-SEARCH over a UDP socket (239.255.255.250:1900) and then fetches
 * each responder's device-description XML over plain HTTP to pull out the
 * friendly name and the AVTransport / RenderingControl SOAP control URLs.
 *
 * Exactly like airplay_mdns this only needs lwIP sockets and must run on the
 * WiFi worker task, so dlna_ssdp_scan() hops onto it via wlan_hal_run_in_worker().
 */

#include <stdbool.h>
#include <stdint.h>

#define DLNA_NAME_MAX    48
#define DLNA_PATH_MAX    128
#define DLNA_MAX_DEVICES 16

typedef struct {
    char name[DLNA_NAME_MAX]; /* <friendlyName> from the device description */
    uint32_t ip; /* network byte order */
    uint16_t port; /* HTTP port of the device description / SOAP endpoint */
    char av_control[DLNA_PATH_MAX]; /* AVTransport controlURL (path, may be abs) */
    char rc_control[DLNA_PATH_MAX]; /* RenderingControl controlURL ("" if none) */
    bool has_cast; /* device advertises DIAL/Google-Cast (→ reachable on :8009) */
} DlnaDevice;

/* Discover DLNA MediaRenderers. Blocks up to ~timeout_ms collecting SSDP
 * replies, then fetches + parses each device description. Fills up to `max`
 * entries in `out`, returns the count found. Requires wlan_hal_start() + a
 * live STA connection. */
int dlna_ssdp_scan(DlnaDevice* out, int max, uint32_t timeout_ms);

#endif /* DLNA_SSDP_H */

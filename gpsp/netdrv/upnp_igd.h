/* upnp_igd.h -- a minimal UPnP Internet Gateway Device client: ask a home
 * router to forward one UDP port to this console and to say what its public
 * address is, so the host of a direct-IP room (transport_lan, mode B) can
 * show "203.0.113.7:19019" instead of asking the player to configure his
 * router.
 *
 * Written for this project from the UPnP Device Architecture and the
 * WANIPConnection:1 service description; no code from any other client.
 * Plain BSD sockets, no XML library (the three values it needs are found by
 * targeted text search), no threads and no blocking call: everything
 * advances inside upnp_igd_poll(), which the main loop calls once per frame.
 *
 * What it does, in order, each step with its own timeout:
 *   1. SSDP: M-SEARCH to 239.255.255.250:1900 for an InternetGatewayDevice,
 *      repeated a few times; the answer carries the description's URL.
 *   2. HTTP GET of the description; find the control URL of
 *      WANIPConnection:1/:2 or WANPPPConnection:1.
 *   3. SOAP GetExternalIPAddress.
 *   4. SOAP AddPortMapping (UDP, external port -> this console's port, a
 *      2 hour lease; retried without a lease for routers that only do
 *      permanent ones, and on the next external ports if the first is
 *      taken by another device).
 *   -> UPNP_IGD_MAPPED.  Later: upnp_igd_remove() -> DeletePortMapping.
 *
 * It is a convenience, not a dependency: many routers have UPnP switched
 * off, and then the state ends UPNP_IGD_FAILED with a reason to show and
 * the room still works for anyone who forwards the port by hand.  Carrier-
 * grade NAT defeats it silently: the mapping succeeds on the home router
 * and the "public" address it reports is itself private (10.x, 100.64.x):
 * upnp_igd_external_is_private() tells the UI to warn.
 *
 * HARDWARE: tested on a PC against netdrv/tests/fake_igd.py only; compiled
 * and linked for the Vita.  Never run against a real router or on a console.
 */
#ifndef UPNP_IGD_H
#define UPNP_IGD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
   UPNP_IGD_SEARCHING = 0,   /* SSDP: looking for a gateway */
   UPNP_IGD_DESCRIBING,      /* fetching its description */
   UPNP_IGD_ASKING_IP,       /* GetExternalIPAddress */
   UPNP_IGD_MAPPING,         /* AddPortMapping */
   UPNP_IGD_MAPPED,          /* done: the port is open */
   UPNP_IGD_REMOVING,        /* DeletePortMapping in flight */
   UPNP_IGD_REMOVED,         /* done: the port is closed again */
   UPNP_IGD_FAILED           /* upnp_igd_error() says why */
};

typedef struct upnp_igd upnp_igd;

/* Start.  local_ip_be: this console's address (network order), which the
 * router forwards to.  internal_port: the port the transport is bound to
 * (lan_transport_port()).  external_port: the port to open on the router,
 * normally the same.  description: what the router's own page will list
 * (ASCII; NULL = "GBAdhoc").  NULL only if out of memory / no socket. */
upnp_igd *upnp_igd_open(uint32_t local_ip_be, uint16_t internal_port,
                        uint16_t external_port, const char *description);
/* Advance; never blocks (a few non-blocking socket calls).  Returns the
 * state.  Call once per frame until MAPPED or FAILED; after that it is
 * free. */
int  upnp_igd_poll(upnp_igd *u);
int  upnp_igd_state(const upnp_igd *u);
/* The router's public address ("" until known; it is known from
 * UPNP_IGD_MAPPING on, and kept if the mapping then fails). */
const char *upnp_igd_external_ip(const upnp_igd *u);
/* The external port actually mapped (may be external_port+1.. if that was
 * taken); 0 until MAPPED. */
uint16_t upnp_igd_external_port(const upnp_igd *u);
/* 1 if the "public" address is in a private or carrier-grade-NAT range:
 * the router is itself behind another NAT and the room is NOT reachable
 * from the internet however well the mapping went. */
int  upnp_igd_external_is_private(const upnp_igd *u);
/* A short reason for FAILED ("" otherwise): for a toast and the log. */
const char *upnp_igd_error(const upnp_igd *u);
/* From MAPPED: ask the router to delete the mapping.  Keep polling until
 * REMOVED or FAILED (about one frame on a LAN).  A no-op in other states. */
void upnp_igd_remove(upnp_igd *u);
/* Free everything now, whatever the state (a mapping that was not removed
 * expires with its lease). */
void upnp_igd_close(upnp_igd *u);

/* Tests: send the M-SEARCH to this address instead of the multicast group
 * (the fake gateway on the loopback), and shorten every timeout to
 * `timeout_ms`.  Call right after upnp_igd_open(). */
void upnp_igd_test_target(upnp_igd *u, uint32_t ssdp_ip_be, uint16_t ssdp_port,
                          unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* UPNP_IGD_H */

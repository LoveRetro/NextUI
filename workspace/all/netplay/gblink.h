/*
 * NextUI GB Link Module
 * Implements GB/GBC Link Cable emulation over WiFi via gambatte core options
 *
 * This module manages gambatte's built-in network serial (HAVE_NETWORK=1)
 * by setting core options programmatically. Gambatte handles the actual
 * TCP connection internally - this module provides:
 * - UDP host discovery (similar to GBA Link)
 * - Core option management for link mode and IP configuration
 * - Consistent UI with GBA Link menus
 *
 * Unlike GBA Link (which uses netpacket callbacks), GB Link:
 * - Gambatte manages TCP connection internally (port 56400)
 * - We set gambatte_gb_link_mode and IP digit options
 * - Each device runs its own save file
 */

#ifndef GBLINK_H
#define GBLINK_H

#include <stdint.h>
#include <stdbool.h>

#define GBLINK_DEFAULT_PORT 56400
#define GBLINK_DISCOVERY_PORT 56421
#define GBLINK_MAGIC "GBLC"
#define GBLINK_PROTOCOL_VERSION 1
#define GBLINK_MAX_GAME_NAME 64
#define GBLINK_MAX_HOSTS 8

typedef enum {
    GBLINK_OFF = 0,
    GBLINK_HOST,
    GBLINK_CLIENT
} GBLinkMode;

typedef enum {
    GBLINK_STATE_IDLE = 0,
    GBLINK_STATE_WAITING,      // Host waiting for client
    GBLINK_STATE_CONNECTING,   // Client connecting to host
    GBLINK_STATE_CONNECTED,    // Connected
    GBLINK_STATE_DISCONNECTED,
    GBLINK_STATE_ERROR
} GBLinkState;

typedef struct {
    char game_name[GBLINK_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
} GBLinkHostInfo;

// Initialize/cleanup
void GBLink_init(void);
void GBLink_quit(void);

// Check if a core supports GB Link (link cable via gambatte)
// core_name is derived from the .so filename (e.g., "gambatte" from "gambatte_libretro.so")
// Returns true if supported (gambatte), also sets internal support flag
bool GBLink_checkCoreSupport(const char* core_name);

// Host mode (sets gambatte_gb_link_mode = "Network Server")
// If hotspot_ip is NULL, uses WiFi mode. Otherwise, uses hotspot mode with given IP.
int GBLink_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip);
int GBLink_stopHost(void);
int GBLink_stopHostFast(void);
void GBLink_stopBroadcast(void);  // Stop UDP broadcast but keep host session active

// Client mode (sets gambatte_gb_link_mode = "Network Client" + IP options)
int GBLink_connectToHost(const char* ip, uint16_t port);
int GBLink_stopClient(void);

// Stop all GB Link activity
// Use this for clean shutdown before quit
void GBLink_stopAll(void);
void GBLink_stopAllFast(void);

// Status queries
GBLinkMode GBLink_getMode(void);
GBLinkState GBLink_getState(void);
bool GBLink_isConnected(void);
const char* GBLink_getStatusMessage(void);
const char* GBLink_getLocalIP(void);
bool GBLink_hasNetworkConnection(void);

// Hotspot mode
bool GBLink_isUsingHotspot(void);

// Host discovery (for client)
int GBLink_startDiscovery(void);
void GBLink_stopDiscovery(void);
int GBLink_getDiscoveredHosts(GBLinkHostInfo* hosts, int max_hosts);

// Core option management - called by minarch to configure gambatte
// Sets gambatte_gb_link_mode to "Network Server"
void GBLink_setCoreOptionsForHost(void);
// Sets gambatte_gb_link_mode to "Network Client" and IP digit options
void GBLink_setCoreOptionsForClient(const char* ip);
// Sets gambatte_gb_link_mode to "Not Connected"
void GBLink_setCoreOptionsDisconnect(void);

// Updates connection state. Called internally by the poll below; exposed so the
// state machine can be driven from one place.
void GBLink_notifyConnectionFromCore(bool connected);

// Observe gambatte's link socket (via the kernel socket table) and update the
// connection state. Throttled, so it is safe to call every frame and from the
// menu/wait loops. Called lazily from GBLink_getState()/GBLink_isConnected() and
// once per frame from minarch's main loop for in-game disconnect detection.
void GBLink_pollConnectionState(void);

// Minarch accessor and utility functions
#include "minarch.h"

#endif /* GBLINK_H */

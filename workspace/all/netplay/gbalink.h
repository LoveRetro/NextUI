/*
 * NextUI GBA Link Module
 * Implements GBA Wireless Adapter (RFU) emulation over WiFi using libretro netpacket interface
 *
 * This module bridges gpSP's built-in RFU emulation with network transport,
 * enabling Pokemon trading, battles (Union Room), and other wireless features.
 *
 * This is separate from netplay (input sync) - it provides wireless adapter
 * communication for GBA games via gpSP's complete RFU implementation.
 */

#ifndef GBALINK_H
#define GBALINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libretro-common/include/libretro.h"

#define GBALINK_DEFAULT_PORT 55437
#define GBALINK_DISCOVERY_PORT 55438
#define GBALINK_MAGIC "GBLK"
#define GBALINK_PROTOCOL_VERSION 1
#define GBALINK_MAX_GAME_NAME 64
#define GBALINK_MAX_HOSTS 8

typedef enum {
    GBALINK_OFF = 0,
    GBALINK_HOST,
    GBALINK_CLIENT
} GBALinkMode;

typedef enum {
    GBALINK_CONN_WIFI = 0,    // Use existing WiFi network
    GBALINK_CONN_HOTSPOT      // Create/connect to hotspot
} GBALinkConnMethod;

typedef enum {
    GBALINK_STATE_IDLE = 0,
    GBALINK_STATE_WAITING,      // Host waiting for client
    GBALINK_STATE_CONNECTING,   // Client connecting to host
    GBALINK_STATE_CONNECTED,    // Connected and ready for SIO packets
    GBALINK_STATE_DISCONNECTED,
    GBALINK_STATE_ERROR
} GBALinkState;

// Return codes for GBALink_connectToHost
#define GBALINK_CONNECT_OK           0   // Connected successfully
#define GBALINK_CONNECT_ERROR       -1   // Connection failed
#define GBALINK_CONNECT_NEEDS_RELOAD 1   // Connected but link mode differs, needs game reload

typedef struct {
    char game_name[GBALINK_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
    char link_mode[32];  // Host's link mode for compatibility check (e.g., "mul_poke", "rfu")
} GBALinkHostInfo;

// Initialize/cleanup
void GBALink_init(void);
void GBALink_quit(void);

// Check if a core supports GBA Link (RFU/Wireless Adapter)
// core_name is derived from the .so filename (e.g., "gpsp" from "gpsp_libretro.so")
// Returns true if supported (gpsp), also sets internal support flag
bool GBALink_checkCoreSupport(const char* core_name);

// Link mode synchronization - host captures mode, client receives and applies it
// Called before hosting to capture the current gpsp_serial value
void GBALink_setLinkMode(const char* mode);
const char* GBALink_getLinkMode(void);

// Pending link mode after GBALINK_CONNECT_NEEDS_RELOAD
// Client's current mode and host's mode that differs
const char* GBALink_getPendingLinkMode(void);    // Returns host's mode (what to change to)
const char* GBALink_getClientLinkMode(void);     // Returns client's current mode
void GBALink_clearPendingReload(void);           // Clear pending reload state
void GBALink_applyPendingLinkMode(void);         // Apply pending mode to config

// Connection management
// If hotspot_ip is NULL, uses WiFi mode. Otherwise, uses hotspot mode with given IP.
// link_mode is the gpsp_serial value to sync with client (can be NULL)
int GBALink_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip, const char* link_mode);
int GBALink_stopHost(void);
int GBALink_stopHostFast(void);
int GBALink_connectToHost(const char* ip, uint16_t port);
void GBALink_disconnect(void);

// Hotspot mode
bool GBALink_isUsingHotspot(void);

// Status queries
GBALinkMode GBALink_getMode(void);
GBALinkState GBALink_getState(void);
bool GBALink_isConnected(void);
const char* GBALink_getStatusMessage(void);
void GBALink_getStatusMessageSafe(char* buf, size_t buf_size);
const char* GBALink_getLocalIP(void);
bool GBALink_hasNetworkConnection(void);

// Host discovery (for client)
int GBALink_startDiscovery(void);
void GBALink_stopDiscovery(void);
int GBALink_getDiscoveredHosts(GBALinkHostInfo* hosts, int max_hosts);

// Direct link mode query (for hotspot mode where broadcasts may not work)
// Sends UDP query directly to host_ip and waits for response
// Returns 0 on success, -1 on failure/timeout
int GBALink_queryHostLinkMode(const char* host_ip, char* link_mode_out, size_t size);

// Netpacket interface callbacks for core
// These are called when the core registers its netpacket interface
void GBALink_onNetpacketStart(uint16_t client_id,
                               retro_netpacket_send_t send_fn,
                               retro_netpacket_poll_receive_t poll_receive_fn);
void GBALink_onNetpacketStop(void);
void GBALink_onNetpacketPoll(void);

// Called by frontend to provide send/poll functions to core
// These wrap the network transport layer
void GBALink_sendPacket(int flags, const void* buf, size_t len, uint16_t client_id);
void GBALink_pollReceive(void);

// Update function (call periodically for connection handling)
void GBALink_update(void);

// Set core netpacket callbacks (called by minarch when core registers RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE)
void GBALink_setCoreCallbacks(const struct retro_netpacket_callback* callbacks);

// Connection state change notifications (called internally by gbalink)
void GBALink_notifyConnected(int is_host);
void GBALink_notifyDisconnected(void);

// Netpacket bridging
bool GBALink_isNetpacketActive(void);
void GBALink_pollAndDeliverPackets(void);  // Call each frame before core.run()

#endif /* GBALINK_H */

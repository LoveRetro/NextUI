/*
 * NextUI Netplay Module
 * Based on RetroArch netplay architecture
 * Implements frame-synchronized multiplayer over WiFi
 */

#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "network_common.h"

#define NETPLAY_DEFAULT_PORT 55435
#define NETPLAY_DISCOVERY_PORT 55436
#define NETPLAY_MAGIC "NXNP"
#define NETPLAY_PROTOCOL_VERSION 2
#define NETPLAY_MAX_GAME_NAME 64
#define NETPLAY_MAX_HOSTS 8

// Frame buffer for rollback (power of 2 for efficient wraparound)
#define NETPLAY_FRAME_BUFFER_SIZE 64
#define NETPLAY_FRAME_MASK (NETPLAY_FRAME_BUFFER_SIZE - 1)

// Stall/timeout constants - extended for reliability on lossy networks
// Pokemon games can pause 2+ seconds during save operations
#define NETPLAY_STALL_TIMEOUT_FRAMES 180      // 3 seconds at 60fps (was 30 frames/500ms)
#define NETPLAY_STALL_WARNING_FRAMES 60       // 1 second warning before disconnect
#define NETPLAY_KEEPALIVE_INTERVAL_FRAMES 30  // Send keepalive every 500ms during stall

// Hotspot SSID prefix - use unified prefix for all link types
#define NETPLAY_HOTSPOT_SSID_PREFIX LINK_HOTSPOT_SSID_PREFIX

typedef enum {
    NETPLAY_CONN_WIFI = 0,
    NETPLAY_CONN_HOTSPOT
} NetplayConnMethod;

// Input latency frames (to hide network jitter)
#define NETPLAY_INPUT_LATENCY_FRAMES 2

typedef enum {
    NETPLAY_OFF = 0,
    NETPLAY_HOST,
    NETPLAY_CLIENT
} NetplayMode;

typedef enum {
    NETPLAY_STATE_IDLE = 0,
    NETPLAY_STATE_WAITING,      // Host waiting for client
    NETPLAY_STATE_CONNECTING,   // Client connecting to host
    NETPLAY_STATE_SYNCING,      // Exchanging initial state
    NETPLAY_STATE_PLAYING,      // Active gameplay
    NETPLAY_STATE_STALLED,      // Waiting for remote input
    NETPLAY_STATE_PAUSED,       // Local or remote player has paused (menu open)
    NETPLAY_STATE_DISCONNECTED,
    NETPLAY_STATE_ERROR
} NetplayState;

typedef struct {
    char game_name[NETPLAY_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
} NetplayHostInfo;

// Initialize/cleanup
void Netplay_init(void);
void Netplay_quit(void);

// Check if a core supports netplay (frame-sync)
// core_name is derived from the .so filename (e.g., "fbneo" from "fbneo_libretro.so")
// Returns true if supported
bool Netplay_checkCoreSupport(const char* core_name);

// Connection management
// If hotspot_ip is NULL, uses WiFi mode. Otherwise, uses hotspot mode with given IP.
int Netplay_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip);
int Netplay_stopHost(void);
int Netplay_stopHostFast(void);
void Netplay_stopBroadcast(void);  // Stop UDP broadcast but keep session active
int Netplay_connectToHost(const char* ip, uint16_t port);
void Netplay_disconnect(void);

// Hotspot mode
bool Netplay_isUsingHotspot(void);

// Status queries
NetplayMode Netplay_getMode(void);
NetplayState Netplay_getState(void);
bool Netplay_isConnected(void);
bool Netplay_isActive(void);
const char* Netplay_getStatusMessage(void);
const char* Netplay_getLocalIP(void);
bool Netplay_hasNetworkConnection(void);

// Host discovery (for client)
int Netplay_startDiscovery(void);
void Netplay_stopDiscovery(void);
int Netplay_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts);

// Frame synchronization (RetroArch-style)
// Called at the start of each frame - handles network polling and sync
bool Netplay_preFrame(void);

// Get inputs for a specific player (called by input_state_callback)
uint16_t Netplay_getInputState(unsigned port);

// Get player buttons with netplay handling
// Returns synchronized netplay input if connected, otherwise local_buttons for port 0
uint32_t Netplay_getPlayerButtons(unsigned port, uint32_t local_buttons);

// Set local player's input for current frame
void Netplay_setLocalInput(uint16_t input);

// Called at end of each frame - sends data, advances frame counter
void Netplay_postFrame(void);

// Check if we should skip this frame (stalled waiting for input)
bool Netplay_shouldStall(void);

// Audio control - returns true if audio should be silenced (during stall)
bool Netplay_shouldSilenceAudio(void);

// State synchronization
int Netplay_sendState(const void* data, size_t size);
int Netplay_receiveState(void* data, size_t size);
bool Netplay_needsStateSync(void);
void Netplay_completeStateSync(void);

// Pause/resume for menu (keeps connection alive)
void Netplay_pause(void);           // Called when entering menu
void Netplay_resume(void);          // Called when exiting menu
void Netplay_pollWhilePaused(void); // Call periodically during menu to maintain connection
bool Netplay_isPaused(void);        // Check if paused

// Main loop update - handles state sync and frame synchronization
// Returns: 1 = run frame, 0 = skip frame (stalled/syncing)
// Callbacks are for core serialization (can be NULL if netplay not active)
typedef size_t (*Netplay_SerializeSizeFn)(void);
typedef bool (*Netplay_SerializeFn)(void* data, size_t size);
typedef bool (*Netplay_UnserializeFn)(const void* data, size_t size);

int Netplay_update(uint16_t local_input,
                   Netplay_SerializeSizeFn serialize_size_fn,
                   Netplay_SerializeFn serialize_fn,
                   Netplay_UnserializeFn unserialize_fn);

#endif /* NETPLAY_H */

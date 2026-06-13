/*
 * NextUI Network Common Module
 * Shared networking utilities for netplay and gbalink
 */

#ifndef NETWORK_COMMON_H
#define NETWORK_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>

// Unified SSID prefix for all link hotspots (Netplay, GBALink, GBLink)
#define LINK_HOTSPOT_SSID_PREFIX "NextUI-"

// Configuration for TCP socket setup
typedef struct {
    int buffer_size;           // SO_SNDBUF/SO_RCVBUF size (bytes)
    int recv_timeout_us;       // SO_RCVTIMEO in microseconds (0 = none)
    bool enable_keepalive;     // SO_KEEPALIVE
} NET_TCPConfig;

// Configuration for hotspot SSID generation
typedef struct {
    const char* prefix;        // Use LINK_HOTSPOT_SSID_PREFIX for all link types
    uint32_t seed;             // Random seed (typically game_crc ^ time)
} NET_HotspotConfig;

// Rate-limited broadcast timer
typedef struct {
    struct timeval last_broadcast;
    int interval_us;
} NET_BroadcastTimer;

// Maximum game name length for discovery
#define NET_MAX_GAME_NAME 64
#define NET_MAX_DISCOVERED_HOSTS 8

// Maximum link mode length for discovery
#define NET_MAX_LINK_MODE 32

// Generic discovery packet (wire format)
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t protocol_version;
    uint32_t game_crc;
    uint16_t port;
    char game_name[NET_MAX_GAME_NAME];
    char link_mode[NET_MAX_LINK_MODE];  // Link mode for compatibility check (e.g., "mul_poke", "rfu")
} NET_DiscoveryPacket;

// Generic host info (for discovered hosts list)
typedef struct {
    char game_name[NET_MAX_GAME_NAME];
    char host_ip[16];
    uint16_t port;
    uint32_t game_crc;
    char link_mode[NET_MAX_LINK_MODE];  // Host's link mode for compatibility check
} NET_HostInfo;

//////////////////////////////////////////////////////////////////////////////
// IP Address Utilities
//////////////////////////////////////////////////////////////////////////////

/**
 * Get the local IP address (preferring wlan interfaces)
 * @param ip_out Buffer to receive IP string
 * @param ip_size Size of ip_out buffer (should be at least 16 bytes)
 */
void NET_getLocalIP(char* ip_out, size_t ip_size);

/**
 * Check if device has a valid network connection
 * @return true if connected (has non-0.0.0.0 IP), false otherwise
 */
bool NET_hasConnection(void);

//////////////////////////////////////////////////////////////////////////////
// TCP Socket Configuration
//////////////////////////////////////////////////////////////////////////////

/**
 * Configure TCP socket with common options (TCP_NODELAY, buffer sizes, etc.)
 * @param fd Socket file descriptor
 * @param config Configuration options (NULL for default: 64KB buffers, no timeout)
 */
void NET_configureTCPSocket(int fd, const NET_TCPConfig* config);

//////////////////////////////////////////////////////////////////////////////
// Server Socket Creation
//////////////////////////////////////////////////////////////////////////////

/**
 * Create a listening TCP socket bound to a port
 * @param port Port number to bind to
 * @param error_msg Buffer to receive error message on failure (can be NULL)
 * @param error_size Size of error_msg buffer
 * @return Socket fd on success, -1 on failure
 */
int NET_createListenSocket(uint16_t port, char* error_msg, size_t error_size);

/**
 * Create a UDP socket for broadcasting
 * @return Socket fd on success, -1 on failure
 */
int NET_createBroadcastSocket(void);

/**
 * Create a non-blocking UDP socket for discovery listening
 * Binds to INADDR_ANY on the specified port with SO_REUSEADDR
 * @param port Port to bind to for receiving discovery broadcasts
 * @return Socket fd on success, -1 on failure
 */
int NET_createDiscoveryListenSocket(uint16_t port);

//////////////////////////////////////////////////////////////////////////////
// Hotspot Utilities
//////////////////////////////////////////////////////////////////////////////

/**
 * Generate a hotspot SSID with random suffix
 * Format: "{prefix}XXXX" where XXXX is a random 4-character code
 * @param ssid_out Buffer to receive SSID string
 * @param ssid_size Size of ssid_out buffer (should be at least 33 bytes)
 * @param config Hotspot configuration (prefix and random seed)
 */
void NET_generateHotspotSSID(char* ssid_out, size_t ssid_size, const NET_HotspotConfig* config);

//////////////////////////////////////////////////////////////////////////////
// Broadcast Timer
//////////////////////////////////////////////////////////////////////////////

/**
 * Initialize a broadcast timer for rate-limiting
 * @param timer Timer to initialize
 * @param interval_us Minimum interval between broadcasts in microseconds
 */
void NET_initBroadcastTimer(NET_BroadcastTimer* timer, int interval_us);

/**
 * Check if enough time has passed to broadcast again
 * Updates the timer's last_broadcast time if returning true
 * @param timer Timer to check
 * @return true if should broadcast, false if too soon
 */
bool NET_shouldBroadcast(NET_BroadcastTimer* timer);

//////////////////////////////////////////////////////////////////////////////
// Discovery Utilities
//////////////////////////////////////////////////////////////////////////////

/**
 * Send a discovery broadcast packet
 * @param udp_fd UDP socket file descriptor
 * @param magic Protocol magic number (host byte order - will be converted)
 * @param protocol_version Protocol version
 * @param game_crc Game CRC for matching
 * @param tcp_port TCP port to advertise
 * @param discovery_port UDP port to broadcast to
 * @param game_name Game name string
 * @param link_mode Link mode string (e.g., "mul_poke", "rfu") - can be NULL
 */
void NET_sendDiscoveryBroadcast(int udp_fd, uint32_t magic, uint32_t protocol_version,
                                 uint32_t game_crc, uint16_t tcp_port,
                                 uint16_t discovery_port, const char* game_name,
                                 const char* link_mode);

/**
 * Receive and deduplicate discovery responses
 * @param udp_fd UDP socket file descriptor
 * @param expected_magic Expected magic number (host byte order)
 * @param hosts Array to store discovered hosts
 * @param current_count Pointer to current count (updated in place)
 * @param max_hosts Maximum hosts to store
 * @return Updated host count
 */
int NET_receiveDiscoveryResponses(int udp_fd, uint32_t expected_magic,
                                   NET_HostInfo* hosts, int* current_count,
                                   int max_hosts);

#endif /* NETWORK_COMMON_H */

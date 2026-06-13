/*
 * NextUI GB Link Module
 * Implements GB/GBC Link Cable emulation over WiFi via gambatte core options
 *
 * This module manages gambatte's built-in network serial (HAVE_NETWORK=1)
 * by setting core options programmatically. Unlike GBA Link which handles
 * TCP directly, gambatte manages its own TCP connection - we just configure
 * the core options and provide UDP discovery.
 *
 * Supported features:
 * - Pokemon trading (Red/Blue/Yellow/Gold/Silver/Crystal)
 * - Tetris 2-player
 * - Other link cable games
 */

#define _GNU_SOURCE  // For strcasestr

#include "gblink.h"
#include "netplay_helper.h"
#include "network_common.h"
#include "defines.h"
#include "api.h"
#ifdef HAS_WIFIMG
#include "wifi_direct.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Protocol constants for UDP discovery
#define GL_DISCOVERY_MAGIC   0x47424C43  // "GBLC"
#define GL_DISCOVERY_RESP    0x47424C52  // "GBLR" - GB Link Discovery Response

// Discovery broadcast interval
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Main GB Link state
static struct {
    GBLinkMode mode;
    GBLinkState state;

    // UDP sockets (separate to avoid race conditions)
    int udp_fd;   // UDP socket for discovery broadcasts
    int discovery_fd;   // For client discovery

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Hotspot mode
    bool using_hotspot;

    // Game info
    char game_name[GBLINK_MAX_GAME_NAME];
    uint32_t game_crc;

    // Discovery
    GBLinkHostInfo discovered_hosts[GBLINK_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // Host broadcast thread
    pthread_t broadcast_thread;
    volatile bool broadcast_thread_active;  // Track if thread was created (portable)
    pthread_mutex_t mutex;
    volatile bool running;

    // Status message
    char status_msg[128];

    // Core support flag (true when gambatte is loaded)
    bool has_gambatte_support;

    // Initialization flag (to prevent use-after-quit crashes)
    bool initialized;

    // Quitting flag (to skip core option setting during quit)
    bool quitting;
} gl = {0};

// Forward declarations
static void* broadcast_thread_func(void* arg);
static void GBLink_disconnect(void);

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void GBLink_init(void) {
    if (gl.initialized) {
        return;  // Already initialized
    }

    memset(&gl, 0, sizeof(gl));
    gl.mode = GBLINK_OFF;
    gl.state = GBLINK_STATE_IDLE;
    gl.udp_fd = -1;
    gl.discovery_fd = -1;
    gl.port = GBLINK_DEFAULT_PORT;

    // Use recursive mutex to prevent deadlock if functions re-acquire lock
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gl.mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    snprintf(gl.status_msg, sizeof(gl.status_msg), "GB Link ready");

    gl.initialized = true;
}

void GBLink_quit(void) {
    if (!gl.initialized) {
        return;  // Already quit or never initialized
    }

    // Capture hotspot state before cleanup
    bool was_host = (gl.mode == GBLINK_HOST);
    bool needs_hotspot_cleanup = gl.using_hotspot || gblink_connected_to_hotspot;

    // Set quitting flag to prevent core option changes during shutdown
    // (the core may already be in an invalid state)
    gl.quitting = true;

    // Stop all link activity using fast version
    GBLink_stopAllFast();
    GBLink_stopDiscovery();

    // Handle hotspot cleanup asynchronously
    if (needs_hotspot_cleanup) {
        stopHotspotAndRestoreWiFiAsync(was_host);
        gblink_connected_to_hotspot = 0;
    }

    gl.initialized = false;  // Mark as quit BEFORE destroying mutex
    pthread_mutex_destroy(&gl.mutex);
}

bool GBLink_checkCoreSupport(const char* core_name) {
    // Gambatte supports GB Link via core options (HAVE_NETWORK=1)
    // core_name is derived from the .so filename (e.g., "gambatte" from "gambatte_libretro.so")
    bool supported = strcasecmp(core_name, "gambatte") == 0;
    gl.has_gambatte_support = supported;
    return supported;
}

//////////////////////////////////////////////////////////////////////////////
// Core Option Management
//////////////////////////////////////////////////////////////////////////////

// Set the port option for gambatte
static void GBLink_setCorePort(uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    minarch_setCoreOptionValue("gambatte_gb_link_network_port", port_str);
}

// Set gambatte_gb_link_mode to "Network Server"
void GBLink_setCoreOptionsForHost(void) {
    minarch_beginOptionsBatch();
    GBLink_setCorePort(gl.port);
    minarch_setCoreOptionValue("gambatte_gb_link_mode", "Network Server");
    minarch_endOptionsBatch();
    // Force gambatte to process options and start TCP server immediately
    minarch_forceCoreOptionUpdate();
}

// Set gambatte_gb_link_mode to "Network Client" and configure IP digit options
void GBLink_setCoreOptionsForClient(const char* ip) {
    minarch_beginOptionsBatch();

    GBLink_setCorePort(gl.port);
    minarch_setCoreOptionValue("gambatte_gb_link_mode", "Network Client");

    // Convert IP address to 12 digits for gambatte's options
    // IP like "192.168.1.100" becomes digits: 1,9,2,1,6,8,0,0,1,1,0,0
    // Gambatte expects the IP formatted as 12 individual digit options
    char digits[13] = {0};  // 12 digits + null
    int d = 0;

    // Extract digits from IP (skip dots, pad each octet to 3 digits)
    char ip_copy[16];
    strncpy(ip_copy, ip, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';

    char* token = strtok(ip_copy, ".");
    while (token && d < 12) {
        int octet = atoi(token);
        // Validate octet range
        if (octet < 0 || octet > 255) {
            LOG_warn("GBLink: Invalid IP octet: %d\n", octet);
            minarch_endOptionsBatch();  // End batch even on error
            return;
        }
        // Format as 3 digits with leading zeros
        digits[d++] = '0' + (octet / 100);
        digits[d++] = '0' + ((octet / 10) % 10);
        digits[d++] = '0' + (octet % 10);
        token = strtok(NULL, ".");
    }

    // Pad remaining with zeros if needed
    while (d < 12) {
        digits[d++] = '0';
    }

    // Set each IP digit option
    for (int i = 0; i < 12; i++) {
        char key[64];
        char val[2] = {digits[i], '\0'};
        snprintf(key, sizeof(key), "gambatte_gb_link_network_server_ip_%d", i + 1);
        minarch_setCoreOptionValue(key, val);
    }

    minarch_endOptionsBatch();
    minarch_forceCoreOptionUpdate();
}

// Set gambatte_gb_link_mode to "Not Connected" and reset IP digits
void GBLink_setCoreOptionsDisconnect(void) {
    // Skip if we're quitting - the core may be in an invalid state
    // and setting options could cause a segfault
    if (gl.quitting) {
        return;
    }

    minarch_beginOptionsBatch();

    minarch_setCoreOptionValue("gambatte_gb_link_mode", "Not Connected");

    // Reset IP digit options to default (0)
    for (int i = 0; i < 12; i++) {
        char key[64];
        snprintf(key, sizeof(key), "gambatte_gb_link_network_server_ip_%d", i + 1);
        minarch_setCoreOptionValue(key, "0");
    }

    minarch_endOptionsBatch();
}

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int GBLink_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip) {
    GBLink_init();  // Lazy init
    if (gl.mode != GBLINK_OFF) {
        return -1;
    }

    // Set up IP based on mode
    if (hotspot_ip) {
        gl.using_hotspot = true;
        strncpy(gl.local_ip, hotspot_ip, sizeof(gl.local_ip) - 1);
        gl.local_ip[sizeof(gl.local_ip) - 1] = '\0';
    } else {
        // WiFi mode - refresh local IP in case it changed since init
        NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    }

    // Create UDP socket for discovery broadcasts
    gl.udp_fd = NET_createBroadcastSocket();
    if (gl.udp_fd < 0) {
        if (hotspot_ip) {
            gl.using_hotspot = false;
        }
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Failed to create broadcast socket");
        return -1;
    }

    strncpy(gl.game_name, game_name, GBLINK_MAX_GAME_NAME - 1);
    gl.game_crc = game_crc;

    // Start broadcast thread for discovery
    gl.running = true;
    pthread_create(&gl.broadcast_thread, NULL, broadcast_thread_func, NULL);
    gl.broadcast_thread_active = true;

    gl.mode = GBLINK_HOST;
    gl.state = GBLINK_STATE_WAITING;

    // Set gambatte core options to start TCP server
    GBLink_setCoreOptionsForHost();

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Hosting on %s:%d", gl.local_ip, gl.port);
    return 0;
}

// Internal helper - stops host with optional hotspot cleanup
static int GBLink_stopHostInternal(bool skip_hotspot_cleanup) {
    if (gl.mode != GBLINK_HOST) return -1;

    // Stop broadcast thread and close UDP socket
    GBLink_stopBroadcast();

    // Stop hotspot if it was started
    if (gl.using_hotspot) {
        if (!skip_hotspot_cleanup) {
#ifdef HAS_WIFIMG
            WIFI_direct_stopHotspot();
            WIFI_direct_restorePreviousConnection();
#endif
        }
        gl.using_hotspot = false;
    }

    // Reset core options and state
    GBLink_disconnect();
    return 0;
}

int GBLink_stopHost(void) {
    return GBLink_stopHostInternal(false);
}

int GBLink_stopHostFast(void) {
    return GBLink_stopHostInternal(true);
}

void GBLink_stopBroadcast(void) {
    // Stop broadcast thread (but keep host session active)
    gl.running = false;
    if (gl.broadcast_thread_active) {
        pthread_join(gl.broadcast_thread, NULL);
        gl.broadcast_thread_active = false;
    }

    // Close UDP socket - no longer needed after connection
    if (gl.udp_fd >= 0) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
    }
}

// Restart UDP broadcast when going back to waiting state
// Called when client disconnects but host wants to accept new clients
static void GBLink_restartBroadcast(void) {
    if (gl.broadcast_thread_active) return;  // Already running
    if (gl.mode != GBLINK_HOST) return;  // Only for host

    // Create UDP socket for discovery broadcasts
    gl.udp_fd = NET_createBroadcastSocket();
    if (gl.udp_fd < 0) {
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Failed to restart broadcast");
        return;
    }

    // Start broadcast thread
    gl.running = true;
    pthread_create(&gl.broadcast_thread, NULL, broadcast_thread_func, NULL);
    gl.broadcast_thread_active = true;
}

// Broadcast thread - sends discovery packets for clients to find
static void* broadcast_thread_func(void* arg) {
    (void)arg;

    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (gl.running && gl.udp_fd >= 0) {
        if (gl.state == GBLINK_STATE_WAITING || gl.state == GBLINK_STATE_CONNECTED) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                NET_sendDiscoveryBroadcast(gl.udp_fd, GL_DISCOVERY_RESP, GBLINK_PROTOCOL_VERSION,
                                           gl.game_crc, gl.port, GBLINK_DISCOVERY_PORT,
                                           gl.game_name, NULL);  // GBLink doesn't use link_mode
            }
        }
        usleep(100000);  // 100ms sleep
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int GBLink_connectToHost(const char* ip, uint16_t port) {
    GBLink_init();  // Lazy init
    if (gl.mode != GBLINK_OFF) {
        return -1;
    }

    // Refresh local IP - important when client just connected to hotspot WiFi
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));

    strncpy(gl.remote_ip, ip, sizeof(gl.remote_ip) - 1);
    gl.port = port;

    // Set mode BEFORE setCoreOptionsForClient so log messages during
    // minarch_forceCoreOptionUpdate() are processed correctly
    gl.mode = GBLINK_CLIENT;
    gl.state = GBLINK_STATE_CONNECTING;

    // Set gambatte core options for client mode (this calls minarch_forceCoreOptionUpdate)
    GBLink_setCoreOptionsForClient(ip);

    // If connection succeeded during the core.run(), state will be CONNECTED
    // Otherwise, it stays CONNECTING and we assume gambatte will connect on resume
    if (gl.state != GBLINK_STATE_CONNECTED) {
        gl.state = GBLINK_STATE_CONNECTED;  // Assume success - gambatte handles TCP
    }

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connected to %s", ip);
    return 0;
}

int GBLink_stopClient(void) {
    if (gl.mode != GBLINK_CLIENT) return -1;

    GBLink_disconnect();
    return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Internal disconnect (resets core options and state)
//////////////////////////////////////////////////////////////////////////////

static void GBLink_disconnect(void) {
    if (!gl.initialized) {
        return;  // Already quit - mutex is destroyed
    }

    pthread_mutex_lock(&gl.mutex);

    // Reset core options and force gambatte to process them
    GBLink_setCoreOptionsDisconnect();
    if (!gl.quitting) {
        minarch_forceCoreOptionUpdate();
    }

    // Reset state
    gl.mode = GBLINK_OFF;
    gl.state = GBLINK_STATE_DISCONNECTED;
    strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
    gl.local_ip[sizeof(gl.local_ip) - 1] = '\0';
    snprintf(gl.status_msg, sizeof(gl.status_msg), "Disconnected");

    pthread_mutex_unlock(&gl.mutex);
}

void GBLink_stopAll(void) {
    if (gl.mode == GBLINK_OFF) return;

    if (gl.mode == GBLINK_HOST) {
        GBLink_stopHost();
    } else if (gl.mode == GBLINK_CLIENT) {
        GBLink_stopClient();
    }
}

void GBLink_stopAllFast(void) {
    if (gl.mode == GBLINK_OFF) return;

    if (gl.mode == GBLINK_HOST) {
        GBLink_stopHostFast();
    } else if (gl.mode == GBLINK_CLIENT) {
        GBLink_stopClient();
    }
}

//////////////////////////////////////////////////////////////////////////////
// Discovery (for clients)
//////////////////////////////////////////////////////////////////////////////

int GBLink_startDiscovery(void) {
    if (gl.discovery_active) return 0;

    gl.discovery_fd = NET_createDiscoveryListenSocket(GBLINK_DISCOVERY_PORT);
    if (gl.discovery_fd < 0) return -1;

    gl.num_hosts = 0;
    gl.discovery_active = true;
    return 0;
}

void GBLink_stopDiscovery(void) {
    if (!gl.discovery_active) return;

    if (gl.discovery_fd >= 0) {
        close(gl.discovery_fd);
        gl.discovery_fd = -1;
    }

    gl.discovery_active = false;
}

int GBLink_getDiscoveredHosts(GBLinkHostInfo* hosts, int max_hosts) {
    if (!gl.discovery_active || gl.discovery_fd < 0) return 0;

    // Poll for discovery responses using shared function
    // GBLinkHostInfo and NET_HostInfo have identical layouts
    NET_receiveDiscoveryResponses(gl.discovery_fd, GL_DISCOVERY_RESP,
                                   (NET_HostInfo*)gl.discovered_hosts, &gl.num_hosts,
                                   GBLINK_MAX_HOSTS);

    int count = (gl.num_hosts < max_hosts) ? gl.num_hosts : max_hosts;
    memcpy(hosts, gl.discovered_hosts, count * sizeof(GBLinkHostInfo));
    return count;
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

GBLinkMode GBLink_getMode(void) { return gl.mode; }

GBLinkState GBLink_getState(void) {
    if (!gl.initialized) return GBLINK_STATE_IDLE;
    GBLink_pollConnectionState(); // refresh from the socket table (throttled, self-locking)
    pthread_mutex_lock(&gl.mutex);
    GBLinkState state = gl.state;
    pthread_mutex_unlock(&gl.mutex);
    return state;
}

bool GBLink_isConnected(void) {
    if (!gl.initialized) return false;
    GBLink_pollConnectionState(); // refresh from the socket table (throttled, self-locking)
    pthread_mutex_lock(&gl.mutex);
    bool connected = (gl.state == GBLINK_STATE_CONNECTED);
    pthread_mutex_unlock(&gl.mutex);
    return connected;
}

const char* GBLink_getStatusMessage(void) { return gl.status_msg; }

const char* GBLink_getLocalIP(void) {
    // Refresh IP if not in an active session (to avoid returning stale hotspot IP)
    if (gl.mode == GBLINK_OFF) {
        NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    }
    return gl.local_ip;
}

bool GBLink_isUsingHotspot(void) { return gl.using_hotspot; }

bool GBLink_hasNetworkConnection(void) {
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    return NET_hasConnection();
}

void GBLink_notifyConnectionFromCore(bool connected) {
    if (!gl.initialized) return;

    pthread_mutex_lock(&gl.mutex);

    if (connected) {
        // Only update if we're in a mode that expects connection
        if (gl.mode == GBLINK_HOST && gl.state == GBLINK_STATE_WAITING) {
            gl.state = GBLINK_STATE_CONNECTED;
            snprintf(gl.status_msg, sizeof(gl.status_msg), "Client connected");
        } else if (gl.mode == GBLINK_CLIENT && gl.state != GBLINK_STATE_CONNECTED) {
            gl.state = GBLINK_STATE_CONNECTED;
            snprintf(gl.status_msg, sizeof(gl.status_msg), "Connected to host");
        }
    } else {
        // Connection lost
        if (gl.state == GBLINK_STATE_CONNECTED) {
            if (gl.mode == GBLINK_HOST) {
                // Host goes back to waiting and restarts broadcast
                gl.state = GBLINK_STATE_WAITING;
                GBLink_restartBroadcast();
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
            } else {
                // Client fully disconnects
                gl.state = GBLINK_STATE_DISCONNECTED;
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Connection lost");
            }
        }
    }

    pthread_mutex_unlock(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Connection State Polling
//////////////////////////////////////////////////////////////////////////////

// gambatte owns the GB Link TCP socket (we only set its mode/port via core
// options), so we observe the connection by inspecting the kernel socket table
// for an ESTABLISHED connection on `port` instead of scraping core log output.
static bool gblink_tcp_established_on_port(uint16_t port) {
    const char* files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    for (int f = 0; f < 2; f++) {
        FILE* fp = fopen(files[f], "r");
        if (!fp) continue;
        char line[512];
        if (fgets(line, sizeof(line), fp)) { // skip header row
            unsigned local_port, rem_port, st;
            while (fgets(line, sizeof(line), fp)) {
                // Columns: sl local_addr:PORT rem_addr:PORT st ... (addr/port in hex)
                if (sscanf(line, "%*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%x %x",
                           &local_port, &rem_port, &st) == 3) {
                    // st 0x01 == TCP_ESTABLISHED. Host: local port == our port;
                    // client: remote port == our port. LISTEN/TIME_WAIT won't match.
                    if (st == 0x01 && (local_port == port || rem_port == port)) {
                        fclose(fp);
                        return true;
                    }
                }
            }
        }
        fclose(fp);
    }
    return false;
}

// Poll gambatte's link socket and feed the state machine. Throttled (~0.5s) so
// it is cheap to call every frame and from the menu/wait loops. Drives the same
// GBLink_notifyConnectionFromCore() transitions the old log scraper did.
void GBLink_pollConnectionState(void) {
    if (!gl.initialized || gl.mode == GBLINK_OFF) return;

    static uint64_t last_us = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
    if (last_us && (now - last_us) < 500000ULL) return; // ~0.5s throttle
    last_us = now;

    GBLink_notifyConnectionFromCore(gblink_tcp_established_on_port(gl.port));
}

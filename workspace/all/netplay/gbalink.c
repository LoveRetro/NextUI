/*
 * NextUI GBA Link Module
 * Implements GBA Wireless Adapter (RFU) emulation over WiFi
 *
 * This module provides a transport layer for the libretro netpacket interface,
 * allowing gpSP to use its built-in Wireless Adapter (RFU) emulation over TCP.
 *
 * gpSP has complete RFU support (rfu.c - 937 lines) that handles the complex
 * wireless adapter protocol used by Pokemon games for trading and battles.
 *
 * Unlike netplay (input synchronization), GBA Link:
 * - Uses gpSP's native RFU timing and protocol
 * - Each device runs its own save file and game state
 * - Only wireless adapter packets are exchanged (not inputs)
 *
 * Supported features via gpSP:
 * - Pokemon trading (FireRed/LeafGreen/Ruby/Sapphire/Emerald)
 * - Pokemon battles (Union Room)
 */

#define _GNU_SOURCE  // For strcasestr

#include "gbalink.h"
#include "minarch.h"
#include "netplay_helper.h"
#include "network_common.h"
#include "defines.h"  // Must come before api.h for BTN_ID_COUNT
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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// Protocol constants
#define GL_PROTOCOL_MAGIC   0x47424C4B  // "GBLK"
#define GL_DISCOVERY_QUERY  0x47424451  // "GBDQ" - GBA Link Discovery Query
#define GL_DISCOVERY_RESP   0x47424452  // "GBDR" - GBA Link Discovery Response

// Discovery broadcast interval
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Network commands
enum {
    CMD_SIO_DATA   = 0x01,  // SIO packet data from core
    CMD_PING       = 0x02,
    CMD_PONG       = 0x03,
    CMD_DISCONNECT = 0x04,
    CMD_READY      = 0x05,  // Signal ready for SIO exchange
    CMD_HEARTBEAT  = 0x06,  // Keepalive during idle periods
};

// Heartbeat interval - RFU protocol requires host to send data so clients can respond
// 500ms interval keeps connection alive without excessive overhead
// (100ms was too aggressive and could overwhelm slow receivers)
#define HEARTBEAT_INTERVAL_MS 500

// Connection timeout - disconnect if no packets received for this long
// 60 seconds provides headroom for:
// - WiFi latency spikes and packet loss
// - Game auto-saves (Pokemon saves when entering Union Room, etc.)
// - Long RFU protocol pauses during room transitions
// Real disconnections are still detected via TCP errors and heartbeat failures
#define GBALINK_CONNECTION_TIMEOUT_MS 60000

// Packet header for TCP communication
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint16_t size;
    uint16_t client_id;  // Source client ID
} PacketHeader;

// Receive buffer for incoming packets
// GBA wireless packets vary in size: trades ~32 bytes, battles ~200 bytes max
// 2048 bytes is sufficient headroom while reducing memory usage
#define RECV_BUFFER_SIZE 2048
typedef struct {
    uint8_t data[RECV_BUFFER_SIZE];
    size_t len;
    uint16_t client_id;
} ReceivedPacket;


// Pending packet queue - needs enough slots to handle burst traffic during
// trade/battle setup. 32 slots with 2KB buffers = 64KB (reduced from 256KB)
#define MAX_PENDING_PACKETS 32

// Main GBA Link state
static struct {
    GBALinkMode mode;
    GBALinkState state;

    // Sockets
    int tcp_fd;         // Main TCP connection
    int listen_fd;      // Server listen socket
    int udp_fd;         // Discovery UDP broadcast socket (for sending)
    int udp_listen_fd;  // Discovery UDP listen socket (for receiving queries)

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Hotspot mode
    GBALinkConnMethod conn_method;
    bool using_hotspot;           // True if hotspot was started for this session
    bool connected_to_hotspot;    // True if client connected to hotspot WiFi

    // Game info
    char game_name[GBALINK_MAX_GAME_NAME];
    uint32_t game_crc;

    // Netpacket interface (from core)
    bool core_registered;
    uint16_t local_client_id;
    retro_netpacket_send_t core_send_fn;        // Stored but we provide our own to core
    retro_netpacket_poll_receive_t core_poll_fn;

    // Receive buffer for delivering to core
    ReceivedPacket pending_packets[MAX_PENDING_PACKETS];
    int pending_count;
    int pending_read_idx;
    int pending_write_idx;

    // Discovery
    GBALinkHostInfo discovered_hosts[GBALINK_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // Threading
    pthread_t listen_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    // Status
    char status_msg[128];

    // Core support flag
    bool has_netpacket_support;

    // Streaming receive buffer for handling partial TCP reads
    // Uses read/write indices to avoid memmove on every packet
    uint8_t stream_buf[RECV_BUFFER_SIZE + sizeof(PacketHeader)];
    size_t stream_buf_read_idx;   // Where to read next packet from
    size_t stream_buf_write_idx;  // Where to write incoming data

    // Heartbeat/keepalive tracking - critical for RFU protocol
    // The host must send data (even dummy) so clients can respond
    struct timeval last_packet_sent;
    struct timeval last_packet_received;

    // Deferred connection notification (listen thread sets, main thread processes)
    // Required because core callbacks must be called from main thread
    volatile bool pending_host_connected;

    // Initialization flag
    bool initialized;

    // Core netpacket callbacks (set by minarch when core registers)
    struct retro_netpacket_callback core_callbacks;
    bool has_core_callbacks;

    // Netpacket bridging state
    bool netpacket_active;
    uint16_t remote_client_id;  // Cached: 1 if we're host, 0 if we're client

    // Link mode synchronization (host's gpsp_serial value sent to client)
    char link_mode[32];

    // Pending reload state (when client's link mode differs from host's)
    bool needs_reload;
    char pending_link_mode[32];   // Host's mode (what to change to)
    char client_link_mode[32];    // Client's current mode

    // Performance optimization: reduce getsockopt frequency
    int error_check_counter;

    // Cached frame time to avoid multiple gettimeofday() calls per frame
    struct timeval frame_time;
    bool frame_time_valid;

    // Deferred disconnect notification (set by recv_packet, processed after mutex release)
    volatile bool pending_disconnect_notify;
} gl = {0};

// Forward declarations
static bool send_packet(uint8_t cmd, const void* data, uint16_t size, uint16_t client_id);
static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms);
static void* listen_thread_func(void* arg);
static void GBALink_sendHeartbeatIfNeeded(const struct timeval* now);

//////////////////////////////////////////////////////////////////////////////
// Performance Optimization Helpers
//////////////////////////////////////////////////////////////////////////////

// Cache frame time - call once at start of frame to avoid multiple gettimeofday() syscalls
static void cache_frame_time(void) {
    gettimeofday(&gl.frame_time, NULL);
    gl.frame_time_valid = true;
}

// Get cached frame time (falls back to fresh call if not cached)
static const struct timeval* get_frame_time(void) {
    if (!gl.frame_time_valid) {
        cache_frame_time();
    }
    return &gl.frame_time;
}

// Invalidate frame time cache (call at end of frame)
static void invalidate_frame_time(void) {
    gl.frame_time_valid = false;
}

// Compact stream buffer if needed - consolidates fragmented buffer space
// Only compacts when read_idx is past halfway point AND we need more space
// This reduces memmove frequency significantly during burst traffic
static void compact_stream_buffer_if_needed(size_t min_space_needed) {
    size_t available = gl.stream_buf_write_idx - gl.stream_buf_read_idx;
    size_t space_at_end = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;

    // Only compact if:
    // 1. We need more space than available at end
    // 2. Read index is past halfway point (worth the memmove cost)
    // 3. There's actually data to move
    if (space_at_end < min_space_needed &&
        gl.stream_buf_read_idx > sizeof(gl.stream_buf) / 2 &&
        available > 0) {
        memmove(gl.stream_buf, gl.stream_buf + gl.stream_buf_read_idx, available);
        gl.stream_buf_read_idx = 0;
        gl.stream_buf_write_idx = available;
    }
}

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void GBALink_init(void) {
    if (gl.initialized) return;

    // Preserve core callbacks - they may have been set before init
    // (core registers callbacks in retro_init, before GBALink session starts)
    struct retro_netpacket_callback saved_callbacks = gl.core_callbacks;
    bool saved_has_callbacks = gl.has_core_callbacks;
    bool saved_has_netpacket = gl.has_netpacket_support;

    memset(&gl, 0, sizeof(gl));

    // Restore core callbacks
    gl.core_callbacks = saved_callbacks;
    gl.has_core_callbacks = saved_has_callbacks;
    gl.has_netpacket_support = saved_has_netpacket;

    gl.mode = GBALINK_OFF;
    gl.state = GBALINK_STATE_IDLE;
    gl.tcp_fd = -1;
    gl.listen_fd = -1;
    gl.udp_fd = -1;
    gl.udp_listen_fd = -1;
    gl.port = GBALINK_DEFAULT_PORT;
    pthread_mutex_init(&gl.mutex, NULL);
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    snprintf(gl.status_msg, sizeof(gl.status_msg), "GBA Link ready");
    gl.initialized = true;
}

void GBALink_quit(void) {
    if (!gl.initialized) return;

    // Capture hotspot state before cleanup (under mutex for our state)
    pthread_mutex_lock(&gl.mutex);
    bool was_host = (gl.mode == GBALINK_HOST);
    bool needs_hotspot_cleanup = gl.using_hotspot;
    pthread_mutex_unlock(&gl.mutex);

    // Read external global separately (it has its own sync in netplay_helper)
    bool client_connected_hotspot = gbalink_connected_to_hotspot;
    needs_hotspot_cleanup = needs_hotspot_cleanup || client_connected_hotspot;

    GBALink_disconnect();
    GBALink_stopHostFast();
    GBALink_stopDiscovery();

    // Handle hotspot cleanup asynchronously
    if (needs_hotspot_cleanup) {
        stopHotspotAndRestoreWiFiAsync(was_host);
        gbalink_connected_to_hotspot = 0;
    }

    pthread_mutex_destroy(&gl.mutex);
    gl.initialized = false;
}

bool GBALink_checkCoreSupport(const char* core_name) {
    // Only gpSP supports Wireless Adapter/RFU via netpacket interface
    // core_name is derived from the .so filename (e.g., "gpsp" from "gpsp_libretro.so")
    bool supported = strcasecmp(core_name, "gpsp") == 0;
    gl.has_netpacket_support = supported;
    return supported;
}

// Set the link mode to synchronize with client
// Called before hosting to capture the current gpsp_serial value
void GBALink_setLinkMode(const char* mode) {
    if (mode) {
        strncpy(gl.link_mode, mode, sizeof(gl.link_mode) - 1);
        gl.link_mode[sizeof(gl.link_mode) - 1] = '\0';
    } else {
        gl.link_mode[0] = '\0';
    }
}

// Get the current link mode (for debugging)
const char* GBALink_getLinkMode(void) {
    return gl.link_mode[0] ? gl.link_mode : NULL;
}

// Get pending link mode (host's mode to change to) after GBALINK_CONNECT_NEEDS_RELOAD
const char* GBALink_getPendingLinkMode(void) {
    return gl.needs_reload && gl.pending_link_mode[0] ? gl.pending_link_mode : NULL;
}

// Get client's current link mode (what it was before host connection)
const char* GBALink_getClientLinkMode(void) {
    return gl.needs_reload && gl.client_link_mode[0] ? gl.client_link_mode : NULL;
}

// Clear pending reload state (called when user cancels)
void GBALink_clearPendingReload(void) {
    gl.needs_reload = false;
    gl.pending_link_mode[0] = '\0';
    gl.client_link_mode[0] = '\0';
}

// Apply pending link mode to config (called when user confirms before reload)
// Note: gpsp ignores runtime option changes, so we just set the option here
// and the caller must reload the game for gpsp to pick it up
void GBALink_applyPendingLinkMode(void) {
    if (gl.needs_reload && gl.pending_link_mode[0]) {
        minarch_setCoreOptionValue("gpsp_serial", gl.pending_link_mode);
        GBALink_clearPendingReload();
    }
}

//////////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////////

// GBALink-specific TCP configuration:
// - 32KB buffers: smaller for faster congestion feedback on WiFi
//   (large buffers hide congestion = bufferbloat, causing delayed failure detection)
//   GBA packets are small (16-104 bytes), so 32KB handles bursts without masking issues
// - 1ms recv timeout for RFU sub-frame timing
// - Keepalive enabled for dead connection detection
static const NET_TCPConfig GBALINK_TCP_CONFIG = {
    .buffer_size = 32768,       // 32KB - smaller for faster WiFi congestion feedback
    .recv_timeout_us = 1000,    // 1ms timeout for RFU timing
    .enable_keepalive = true
};

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int GBALink_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip, const char* link_mode) {
    LOG_info("GBALink: HOST startHost() game=%s hotspot=%s link_mode=%s has_callbacks=%d\n",
             game_name, hotspot_ip ? hotspot_ip : "NULL", link_mode ? link_mode : "NULL",
             gl.has_core_callbacks);
    GBALink_init();  // Lazy init
    if (gl.mode != GBALINK_OFF) {
        LOG_info("GBALink: HOST already in mode %d, aborting\n", gl.mode);
        return -1;
    }

    // Set link mode for client synchronization (must be after init to avoid being cleared)
    GBALink_setLinkMode(link_mode);

    // Set up IP based on mode
    if (hotspot_ip) {
        gl.using_hotspot = true;
        gl.conn_method = GBALINK_CONN_HOTSPOT;
        strncpy(gl.local_ip, hotspot_ip, sizeof(gl.local_ip) - 1);
        gl.local_ip[sizeof(gl.local_ip) - 1] = '\0';
    }

    // Create TCP listen socket using shared utility
    gl.listen_fd = NET_createListenSocket(gl.port, gl.status_msg, sizeof(gl.status_msg));
    if (gl.listen_fd < 0) {
        if (hotspot_ip) {
            gl.using_hotspot = false;
        }
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    gl.udp_fd = NET_createBroadcastSocket();
    if (gl.udp_fd < 0) {
        close(gl.listen_fd);
        gl.listen_fd = -1;
        if (hotspot_ip) {
            gl.using_hotspot = false;
        }
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Failed to create broadcast socket");
        return -1;
    }

    // Create UDP socket for receiving discovery queries (especially for hotspot mode)
    gl.udp_listen_fd = NET_createDiscoveryListenSocket(GBALINK_DISCOVERY_PORT);
    if (gl.udp_listen_fd < 0) {
        LOG_warn("GBALink: Could not create UDP query listener (non-fatal)\n");
        // Non-fatal - broadcasts still work on regular WiFi
    }

    strncpy(gl.game_name, game_name, GBALINK_MAX_GAME_NAME - 1);
    gl.game_crc = game_crc;

    // Start listen thread
    gl.running = true;
    pthread_create(&gl.listen_thread, NULL, listen_thread_func, NULL);

    gl.mode = GBALINK_HOST;
    gl.state = GBALINK_STATE_WAITING;
    gl.local_client_id = 0;  // Host is always client 0

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Hosting on %s:%d", gl.local_ip, gl.port);
    LOG_info("GBALink: HOST listening on %s:%d has_callbacks=%d\n", gl.local_ip, gl.port, gl.has_core_callbacks);
    return 0;
}

// Internal helper - stops host with optional hotspot cleanup
static int GBALink_stopHostInternal(bool skip_hotspot_cleanup) {
    if (gl.mode != GBALINK_HOST) return -1;

    gl.running = false;

    // Close listen socket to unblock accept() in listen thread
    if (gl.listen_fd >= 0) {
        close(gl.listen_fd);
        gl.listen_fd = -1;
    }

    if (gl.listen_thread) {
        // Thread will exit via gl.running check - no pthread_cancel needed
        // (pthread_cancel can leave mutex/fd in undefined state)
        pthread_join(gl.listen_thread, NULL);
        gl.listen_thread = 0;
    }

    if (gl.udp_fd >= 0) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
    }

    if (gl.udp_listen_fd >= 0) {
        close(gl.udp_listen_fd);
        gl.udp_listen_fd = -1;
    }

    GBALink_disconnect();

    // Stop hotspot if it was started
    if (gl.using_hotspot) {
        if (!skip_hotspot_cleanup) {
#ifdef HAS_WIFIMG
            WIFI_direct_stopHotspot();
#endif
        }
        gl.using_hotspot = false;
        // Clear local IP since hotspot is stopping
        // Hotspot stop will restore previous WiFi connection,
        // and the IP will be refreshed when needed
        strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
    }
    gl.mode = GBALINK_OFF;
    gl.state = GBALINK_STATE_IDLE;
    snprintf(gl.status_msg, sizeof(gl.status_msg), "GBA Link ready");
    return 0;
}

int GBALink_stopHost(void) {
    return GBALink_stopHostInternal(false);
}

int GBALink_stopHostFast(void) {
    return GBALink_stopHostInternal(true);
}

// Restart UDP broadcast when going back to waiting state
// Called when client disconnects but host wants to accept new clients
static void GBALink_restartBroadcast(void) {
    if (gl.udp_fd >= 0) return;  // Already running
    if (gl.mode != GBALINK_HOST) return;  // Only for host

    gl.udp_fd = NET_createBroadcastSocket();
    if (gl.udp_fd < 0) {
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Failed to restart broadcast");
    }
}

static void* listen_thread_func(void* arg) {
    (void)arg;

    // Use shared broadcast timer for rate limiting
    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (gl.running && gl.listen_fd >= 0) {
        // Rate-limited discovery broadcast using shared timer
        if (gl.udp_fd >= 0 && gl.state == GBALINK_STATE_WAITING) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                NET_sendDiscoveryBroadcast(gl.udp_fd, GL_DISCOVERY_RESP, GBALINK_PROTOCOL_VERSION,
                                           gl.game_crc, gl.port, GBALINK_DISCOVERY_PORT,
                                           gl.game_name, gl.link_mode);
            }
        }

        // Handle incoming UDP discovery queries (for hotspot mode where broadcasts may not work)
        // Protect UDP socket access with mutex to prevent race with socket closure
        pthread_mutex_lock(&gl.mutex);
        int udp_fd = gl.udp_listen_fd;
        bool in_waiting = (gl.state == GBALINK_STATE_WAITING);
        pthread_mutex_unlock(&gl.mutex);

        if (udp_fd >= 0 && in_waiting) {
            NET_DiscoveryPacket query_pkt;
            struct sockaddr_in sender;
            socklen_t sender_len = sizeof(sender);
            ssize_t recv_len = recvfrom(udp_fd, &query_pkt, sizeof(query_pkt), MSG_DONTWAIT,
                                        (struct sockaddr*)&sender, &sender_len);
            if (recv_len >= (ssize_t)sizeof(query_pkt) && ntohl(query_pkt.magic) == GL_DISCOVERY_QUERY) {
                // Respond directly to the sender with our info
                pthread_mutex_lock(&gl.mutex);
                NET_DiscoveryPacket resp_pkt = {0};
                resp_pkt.magic = htonl(GL_DISCOVERY_RESP);
                resp_pkt.protocol_version = htonl(GBALINK_PROTOCOL_VERSION);
                resp_pkt.game_crc = htonl(gl.game_crc);
                resp_pkt.port = htons(gl.port);
                strncpy(resp_pkt.game_name, gl.game_name, NET_MAX_GAME_NAME - 1);
                strncpy(resp_pkt.link_mode, gl.link_mode, NET_MAX_LINK_MODE - 1);
                int send_fd = gl.udp_listen_fd;  // Re-check under mutex
                pthread_mutex_unlock(&gl.mutex);
                if (send_fd >= 0) {
                    sendto(send_fd, &resp_pkt, sizeof(resp_pkt), 0,
                           (struct sockaddr*)&sender, sender_len);
                }
            }
        }

        // Check for incoming connection
        if (gl.state == GBALINK_STATE_WAITING && gl.listen_fd >= 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(gl.listen_fd, &fds);

            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms timeout
            int sel_result = select(gl.listen_fd + 1, &fds, NULL, NULL, &tv);
            if (sel_result < 0 || !gl.running) break;  // Socket closed or stopping
            if (sel_result > 0) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int fd = accept(gl.listen_fd, (struct sockaddr*)&client_addr, &len);
                if (fd >= 0) {
                    char client_ip[16];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                    LOG_info("GBALink: HOST accept() got connection from %s\n", client_ip);

                    pthread_mutex_lock(&gl.mutex);

                    if (gl.state != GBALINK_STATE_WAITING) {
                        LOG_info("GBALink: HOST rejecting - not in WAITING state\n");
                        close(fd);
                        pthread_mutex_unlock(&gl.mutex);
                        continue;
                    }

                    // Configure TCP socket using GBALink-specific settings
                    NET_configureTCPSocket(fd, &GBALINK_TCP_CONFIG);

                    gl.tcp_fd = fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, gl.remote_ip, sizeof(gl.remote_ip));

                    gl.state = GBALINK_STATE_CONNECTED;
                    gl.pending_count = 0;
                    gl.pending_read_idx = 0;
                    gl.pending_write_idx = 0;
                    gl.stream_buf_read_idx = 0;
                    gl.stream_buf_write_idx = 0;
                    struct timeval now;
                    gettimeofday(&now, NULL);
                    gl.last_packet_sent = now;
                    gl.last_packet_received = now;

                    snprintf(gl.status_msg, sizeof(gl.status_msg), "Client connected: %s", gl.remote_ip);
                    LOG_info("GBALink: HOST waiting for client READY signal...\n");

                    // Wait for client's READY signal
                    bool client_ready = false;
                    for (int attempts = 0; attempts < 100 && gl.running; attempts++) {  // 5 second timeout
                        PacketHeader hdr;
                        uint8_t data[64];
                        bool got_packet = recv_packet(&hdr, data, sizeof(data), 50);
                        if (got_packet && hdr.cmd == CMD_READY) {
                            client_ready = true;
                            break;
                        }
                    }

                    LOG_info("GBALink: HOST client_ready=%d\n", client_ready);
                    if (!client_ready) {
                        LOG_error("GBALink: HOST timeout waiting for client READY\n");
                        // Send DISCONNECT so client knows we rejected them
                        send_packet(CMD_DISCONNECT, NULL, 0, 0);
                        close(gl.tcp_fd);
                        gl.tcp_fd = -1;
                        gl.state = GBALINK_STATE_WAITING;
                        pthread_mutex_unlock(&gl.mutex);
                        continue;
                    }

                    // Send READY back to client with link mode for synchronization
                    // Link mode is sent so client can match host's gpsp_serial setting
                    uint16_t mode_len = gl.link_mode[0] ? (uint16_t)(strlen(gl.link_mode) + 1) : 0;
                    send_packet(CMD_READY, gl.link_mode, mode_len, 0);

                    // Verify state hasn't changed during handshake
                    // (main thread could have disconnected during mutex release in send_packet)
                    if (gl.tcp_fd < 0 || gl.state != GBALINK_STATE_CONNECTED) {
                        // Connection was interrupted during handshake
                        pthread_mutex_unlock(&gl.mutex);
                        continue;
                    }

                    // Set flag for main thread to process (core callbacks must run on main thread)
                    // Memory barrier ensures all state writes are visible before flag is set
                    __sync_synchronize();
                    gl.pending_host_connected = true;
                    LOG_info("GBALink: HOST handshake complete, pending_host_connected=true\n");

                    // Close UDP sockets - no longer needed after connection
                    if (gl.udp_fd >= 0) {
                        close(gl.udp_fd);
                        gl.udp_fd = -1;
                    }
                    if (gl.udp_listen_fd >= 0) {
                        close(gl.udp_listen_fd);
                        gl.udp_listen_fd = -1;
                    }

                    pthread_mutex_unlock(&gl.mutex);
                }
            }
        } else {
            usleep(50000);  // 50ms
        }
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int GBALink_connectToHost(const char* ip, uint16_t port) {
    LOG_info("GBALink: CLIENT connectToHost(%s:%d) called\n", ip, port);
    GBALink_init();  // Lazy init
    if (gl.mode != GBALINK_OFF) {
        LOG_info("GBALink: CLIENT already in mode %d, aborting\n", gl.mode);
        return -1;
    }

    // Refresh local IP - important when client just connected to hotspot WiFi
    // This ensures we have the correct IP from the hotspot's DHCP server
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    LOG_info("GBALink: CLIENT local_ip=%s\n", gl.local_ip);

    gl.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gl.tcp_fd < 0) {
        LOG_info("GBALink: CLIENT socket() failed errno=%d\n", errno);
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Socket creation failed");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Invalid IP address");
        return -1;
    }

    gl.state = GBALINK_STATE_CONNECTING;
    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connecting to %s:%d...", ip, port);

    // Connect with timeout
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(gl.tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(gl.tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_info("GBALink: CLIENT connect() failed errno=%d\n", errno);
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        gl.state = GBALINK_STATE_ERROR;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Connection failed");
        return -1;
    }

    LOG_info("GBALink: CLIENT TCP connected to %s:%d\n", ip, port);

    // Configure TCP socket using GBALink-specific settings
    NET_configureTCPSocket(gl.tcp_fd, &GBALINK_TCP_CONFIG);

    strncpy(gl.remote_ip, ip, sizeof(gl.remote_ip) - 1);
    gl.port = port;
    gl.mode = GBALINK_CLIENT;
    gl.state = GBALINK_STATE_CONNECTED;
    gl.local_client_id = 1;  // Client is always client 1

    gl.pending_count = 0;
    gl.pending_read_idx = 0;
    gl.pending_write_idx = 0;
    gl.stream_buf_read_idx = 0;
    gl.stream_buf_write_idx = 0;
    struct timeval now;
    gettimeofday(&now, NULL);
    gl.last_packet_sent = now;
    gl.last_packet_received = now;

    snprintf(gl.status_msg, sizeof(gl.status_msg), "Connected to %s", ip);

    // Send READY signal to host and wait for host's READY
    pthread_mutex_lock(&gl.mutex);
    send_packet(CMD_READY, NULL, 0, gl.local_client_id);
    pthread_mutex_unlock(&gl.mutex);

    // Set socket receive timeout for handshake (5 seconds)
    // This helps detect dead connections during handshake
    struct timeval handshake_timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(gl.tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &handshake_timeout, sizeof(handshake_timeout));

    // Wait for host's READY signal (with timeout - 5 seconds = 100 x 50ms)
    bool host_ready = false;
    bool needs_reload = false;
    for (int attempts = 0; attempts < 100; attempts++) {
        PacketHeader hdr;
        uint8_t data[64];
        pthread_mutex_lock(&gl.mutex);
        bool got_packet = recv_packet(&hdr, data, sizeof(data), 50);
        pthread_mutex_unlock(&gl.mutex);

        if (got_packet) {
            if (hdr.cmd == CMD_READY) {
                // Extract link mode from payload and check if it differs from client
                if (hdr.size > 0 && hdr.size < sizeof(data)) {
                    data[hdr.size] = '\0';  // Ensure null-terminated
                    const char* host_link_mode = (const char*)data;
                    if (host_link_mode[0]) {
                        // Get client's current link mode
                        const char* client_mode = minarch_getCoreOptionValue("gpsp_serial");

                        // Check if modes differ (need reload for gpsp to pick up new mode)
                        if (!client_mode || strcmp(client_mode, host_link_mode) != 0) {
                            // Store the modes for UI confirmation
                            strncpy(gl.pending_link_mode, host_link_mode, sizeof(gl.pending_link_mode) - 1);
                            gl.pending_link_mode[sizeof(gl.pending_link_mode) - 1] = '\0';
                            strncpy(gl.client_link_mode, client_mode ? client_mode : "auto",
                                    sizeof(gl.client_link_mode) - 1);
                            gl.client_link_mode[sizeof(gl.client_link_mode) - 1] = '\0';
                            gl.needs_reload = true;
                            needs_reload = true;
                        }
                    }
                }
                host_ready = true;
                break;
            } else if (hdr.cmd == CMD_DISCONNECT) {
                // Host rejected us during handshake
                LOG_error("GBALink: Host sent DISCONNECT during handshake\n");
                close(gl.tcp_fd);
                gl.tcp_fd = -1;
                gl.mode = GBALINK_OFF;
                gl.state = GBALINK_STATE_ERROR;
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Host rejected connection");
                return GBALINK_CONNECT_ERROR;
            }
        }
    }

    if (!host_ready) {
        LOG_error("GBALink: CLIENT timeout waiting for host READY\n");
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
        gl.mode = GBALINK_OFF;
        gl.state = GBALINK_STATE_ERROR;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Host not responding");
        return GBALINK_CONNECT_ERROR;
    }

    // Restore normal timeout after successful handshake
    struct timeval normal_timeout = {.tv_sec = 0, .tv_usec = GBALINK_TCP_CONFIG.recv_timeout_us};
    setsockopt(gl.tcp_fd, SOL_SOCKET, SO_RCVTIMEO, &normal_timeout, sizeof(normal_timeout));

    // If link modes differ, return special code so UI can confirm with user
    // Don't start netpacket session yet - wait for user confirmation and reload
    if (needs_reload) {
        return GBALINK_CONNECT_NEEDS_RELOAD;
    }

    // Now both sides are ready - notify minarch to start netpacket session
    GBALink_notifyConnected(0);

    return GBALINK_CONNECT_OK;
}

void GBALink_disconnect(void) {
    GBALinkMode prev_mode = gl.mode;

    // Notify minarch to stop netpacket session first
    GBALink_notifyDisconnected();

    pthread_mutex_lock(&gl.mutex);
    if (gl.tcp_fd >= 0) {
        send_packet(CMD_DISCONNECT, NULL, 0, 0);
        // send_packet re-acquires mutex, so we still hold it here
        close(gl.tcp_fd);
        gl.tcp_fd = -1;
    }

    // Always clear core_registered to prevent timeout checks
    gl.core_registered = false;

    if (prev_mode == GBALINK_CLIENT) {
        gl.mode = GBALINK_OFF;
        gl.state = GBALINK_STATE_DISCONNECTED;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Disconnected");
        // Clear local IP since client is disconnecting from hotspot network
        // The actual WiFi disconnection happens separately, but we clear here
        // so the UI shows no IP while disconnected
        strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
        gl.connected_to_hotspot = false;
    } else if (prev_mode == GBALINK_HOST) {
        gl.state = GBALINK_STATE_WAITING;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
    } else {
        gl.mode = GBALINK_OFF;
        gl.state = GBALINK_STATE_DISCONNECTED;
        snprintf(gl.status_msg, sizeof(gl.status_msg), "Disconnected");
    }

    gl.pending_count = 0;
    gl.stream_buf_read_idx = 0;
    gl.stream_buf_write_idx = 0;
    pthread_mutex_unlock(&gl.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int GBALink_startDiscovery(void) {
    if (gl.discovery_active) return 0;

    gl.udp_fd = NET_createDiscoveryListenSocket(GBALINK_DISCOVERY_PORT);
    if (gl.udp_fd < 0) return -1;

    gl.num_hosts = 0;
    gl.discovery_active = true;
    return 0;
}

void GBALink_stopDiscovery(void) {
    if (!gl.discovery_active) return;

    if (gl.udp_fd >= 0 && gl.mode == GBALINK_OFF) {
        close(gl.udp_fd);
        gl.udp_fd = -1;
    }

    gl.discovery_active = false;
}

int GBALink_getDiscoveredHosts(GBALinkHostInfo* hosts, int max_hosts) {
    if (!gl.discovery_active || gl.udp_fd < 0) return 0;

    // Poll for discovery responses using shared function
    // GBALinkHostInfo and NET_HostInfo have identical layouts
    NET_receiveDiscoveryResponses(gl.udp_fd, GL_DISCOVERY_RESP,
                                   (NET_HostInfo*)gl.discovered_hosts, &gl.num_hosts,
                                   GBALINK_MAX_HOSTS);

    int count = (gl.num_hosts < max_hosts) ? gl.num_hosts : max_hosts;
    memcpy(hosts, gl.discovered_hosts, count * sizeof(GBALinkHostInfo));
    return count;
}

int GBALink_queryHostLinkMode(const char* host_ip, char* link_mode_out, size_t size) {
    if (!host_ip || !link_mode_out || size < 2) return -1;
    link_mode_out[0] = '\0';

    // Create UDP socket for query
    int query_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (query_fd < 0) {
        return -1;
    }

    // Set send and receive timeouts to prevent blocking indefinitely
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};  // 500ms timeout
    setsockopt(query_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(query_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Prepare query packet
    NET_DiscoveryPacket query_pkt = {0};
    query_pkt.magic = htonl(GL_DISCOVERY_QUERY);
    query_pkt.protocol_version = htonl(GBALINK_PROTOCOL_VERSION);

    // Send to host
    struct sockaddr_in host_addr = {0};
    host_addr.sin_family = AF_INET;
    host_addr.sin_port = htons(GBALINK_DISCOVERY_PORT);
    if (inet_pton(AF_INET, host_ip, &host_addr.sin_addr) <= 0) {
        close(query_fd);
        return -1;  // Invalid IP address
    }

    // Try up to 3 times with 500ms timeout each
    for (int attempt = 0; attempt < 3; attempt++) {
        sendto(query_fd, &query_pkt, sizeof(query_pkt), 0,
               (struct sockaddr*)&host_addr, sizeof(host_addr));

        // Wait for response
        NET_DiscoveryPacket resp_pkt;
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t recv_len = recvfrom(query_fd, &resp_pkt, sizeof(resp_pkt), 0,
                                    (struct sockaddr*)&sender, &sender_len);

        if (recv_len >= (ssize_t)sizeof(resp_pkt) && ntohl(resp_pkt.magic) == GL_DISCOVERY_RESP) {
            // Got response - extract link_mode
            strncpy(link_mode_out, resp_pkt.link_mode, size - 1);
            link_mode_out[size - 1] = '\0';
            close(query_fd);
            return 0;
        }
    }

    close(query_fd);
    return -1;
}

//////////////////////////////////////////////////////////////////////////////
// Netpacket Interface (called by frontend when core registers)
//////////////////////////////////////////////////////////////////////////////

void GBALink_onNetpacketStart(uint16_t client_id,
                               retro_netpacket_send_t send_fn,
                               retro_netpacket_poll_receive_t poll_receive_fn) {
    pthread_mutex_lock(&gl.mutex);
    gl.core_registered = true;
    gl.local_client_id = client_id;
    gl.core_send_fn = send_fn;
    gl.core_poll_fn = poll_receive_fn;
    // Reset packet timestamps to start fresh timeout window after handshake
    struct timeval now;
    gettimeofday(&now, NULL);
    gl.last_packet_sent = now;
    gl.last_packet_received = now;
    pthread_mutex_unlock(&gl.mutex);
}

void GBALink_onNetpacketStop(void) {
    pthread_mutex_lock(&gl.mutex);
    gl.core_registered = false;
    gl.core_send_fn = NULL;
    gl.core_poll_fn = NULL;
    pthread_mutex_unlock(&gl.mutex);
}

void GBALink_onNetpacketPoll(void) {
    // Called by core each frame - check for incoming packets
    GBALink_pollReceive();
}

//////////////////////////////////////////////////////////////////////////////
// Packet Sending (called by core via netpacket send function)
//////////////////////////////////////////////////////////////////////////////

void GBALink_sendPacket(int flags, const void* buf, size_t len, uint16_t client_id) {
    if (!GBALink_isConnected()) return;

    // Handle empty flush request (flush only, no data)
    // We use TCP_NODELAY so packets are already flushed immediately
    if (!buf || len == 0) return;

    // Send to remote via TCP
    pthread_mutex_lock(&gl.mutex);
    bool sent_ok = send_packet(CMD_SIO_DATA, buf, (uint16_t)len, client_id);
    if (!sent_ok) {
        LOG_warn("GBALink: SIO_DATA send failed, disconnecting\n");
        pthread_mutex_unlock(&gl.mutex);
        GBALink_disconnect();
        return;
    }
    // Update last_packet_sent to prevent unnecessary heartbeats during active communication
    gettimeofday(&gl.last_packet_sent, NULL);
    pthread_mutex_unlock(&gl.mutex);

    // FLUSH_HINT: Since we use TCP_NODELAY and send immediately,
    // packets are already flushed. No additional action needed.
    (void)flags;
}

// Limit packets per poll to prevent frame stalls during high traffic
// 64 packets allows same-frame delivery of packet bursts during trade/battle
// This prevents Pokemon Union Room trade failures caused by buffering packets until next frame
#define MAX_PACKETS_PER_POLL 64

// Send heartbeat packet if idle for too long (host only)
// Critical for RFU protocol: "the host must send data (even dummy) so clients can respond"
// Without this, clients timeout fatally (unrecoverable "communication error")
// Takes cached frame time to avoid extra gettimeofday() syscalls
static void GBALink_sendHeartbeatIfNeeded(const struct timeval* now) {
    // Only host sends heartbeats - clients respond to host packets
    if (gl.mode != GBALINK_HOST || !GBALink_isConnected()) return;

    long elapsed_ms = (now->tv_sec - gl.last_packet_sent.tv_sec) * 1000 +
                      (now->tv_usec - gl.last_packet_sent.tv_usec) / 1000;

    if (elapsed_ms >= HEARTBEAT_INTERVAL_MS) {
        pthread_mutex_lock(&gl.mutex);
        bool sent_ok = send_packet(CMD_HEARTBEAT, NULL, 0, 0);
        if (sent_ok) {
            gl.last_packet_sent = *now;
        } else {
            // Heartbeat send failed - connection is dead
            GBALink_disconnect();
            return;
        }
        pthread_mutex_unlock(&gl.mutex);
    }
}

void GBALink_pollReceive(void) {
    if (!GBALink_isConnected()) return;

    // Cache frame time once at start - avoids multiple gettimeofday() syscalls
    cache_frame_time();

    // Send heartbeat if needed (host only, keeps clients alive)
    GBALink_sendHeartbeatIfNeeded(get_frame_time());

    pthread_mutex_lock(&gl.mutex);

    // Check for incoming packets with short timeout
    PacketHeader hdr;
    uint8_t data[RECV_BUFFER_SIZE];
    // RECV_BUFFER_SIZE is 2048, well within uint16_t range
    uint16_t max_recv = RECV_BUFFER_SIZE;
    int packets_this_poll = 0;

    while (packets_this_poll < MAX_PACKETS_PER_POLL && recv_packet(&hdr, data, max_recv, 0)) {
        if (hdr.cmd == CMD_SIO_DATA) {
            // Queue packet for delivery to core
            // Note: hdr.size is validated by recv_packet to be <= RECV_BUFFER_SIZE
            if (gl.pending_count < MAX_PENDING_PACKETS && hdr.size <= RECV_BUFFER_SIZE) {
                ReceivedPacket* pkt = &gl.pending_packets[gl.pending_write_idx];
                memcpy(pkt->data, data, hdr.size);
                pkt->len = hdr.size;
                pkt->client_id = hdr.client_id;
                gl.pending_write_idx = (gl.pending_write_idx + 1) % MAX_PENDING_PACKETS;
                gl.pending_count++;
            }
            packets_this_poll++;
        } else if (hdr.cmd == CMD_HEARTBEAT) {
            // Heartbeat received - timestamp already updated in recv_packet
        } else if (hdr.cmd == CMD_DISCONNECT) {
            // Remote sent explicit disconnect command
            GBALinkMode prev_mode = gl.mode;
            close(gl.tcp_fd);
            gl.tcp_fd = -1;

            if (prev_mode == GBALINK_CLIENT) {
                // Client fully disconnects
                gl.mode = GBALINK_OFF;
                gl.state = GBALINK_STATE_DISCONNECTED;
                gl.core_registered = false;  // Ensure this is cleared
                strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
                gl.connected_to_hotspot = false;
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Host disconnected");
                // Notify minarch that connection is lost
                pthread_mutex_unlock(&gl.mutex);
                GBALink_notifyDisconnected();
                pthread_mutex_lock(&gl.mutex);
                // Verify state wasn't corrupted during callback
                if (gl.mode != GBALINK_OFF || gl.state != GBALINK_STATE_DISCONNECTED) {
                    // Force correct state
                    gl.mode = GBALINK_OFF;
                    gl.state = GBALINK_STATE_DISCONNECTED;
                }
            } else if (prev_mode == GBALINK_HOST) {
                // Host goes back to waiting and restarts broadcast
                gl.state = GBALINK_STATE_WAITING;
                gl.core_registered = false;  // Core session is over
                // Notify minarch before restarting broadcast
                pthread_mutex_unlock(&gl.mutex);
                GBALink_notifyDisconnected();
                pthread_mutex_lock(&gl.mutex);
                GBALink_restartBroadcast();
                snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
            }
            break;
        }
    }

    // Check for deferred disconnect notification (set by recv_packet)
    bool need_disconnect_notify = gl.pending_disconnect_notify;
    gl.pending_disconnect_notify = false;

    pthread_mutex_unlock(&gl.mutex);

    // Process deferred notification after mutex release to avoid holding lock during callbacks
    if (need_disconnect_notify) {
        GBALink_notifyDisconnected();
    }
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

GBALinkMode GBALink_getMode(void) {
    if (!gl.initialized) return GBALINK_OFF;
    pthread_mutex_lock(&gl.mutex);
    GBALinkMode mode = gl.mode;
    pthread_mutex_unlock(&gl.mutex);
    return mode;
}

GBALinkState GBALink_getState(void) {
    if (!gl.initialized) return GBALINK_STATE_IDLE;
    pthread_mutex_lock(&gl.mutex);
    GBALinkState state = gl.state;
    pthread_mutex_unlock(&gl.mutex);
    return state;
}

bool GBALink_isConnected(void) {
    if (!gl.initialized) return false;
    pthread_mutex_lock(&gl.mutex);
    bool connected = gl.tcp_fd >= 0 && gl.state == GBALINK_STATE_CONNECTED;
    pthread_mutex_unlock(&gl.mutex);
    return connected;
}

const char* GBALink_getStatusMessage(void) { return gl.status_msg; }

void GBALink_getStatusMessageSafe(char* buf, size_t buf_size) {
    if (!gl.initialized) {
        strncpy(buf, "Not initialized", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }
    pthread_mutex_lock(&gl.mutex);
    strncpy(buf, gl.status_msg, buf_size - 1);
    buf[buf_size - 1] = '\0';
    pthread_mutex_unlock(&gl.mutex);
}

// Thread-safe version that copies IP to caller's buffer
void GBALink_getLocalIPSafe(char* buf, size_t buf_size) {
    if (!gl.initialized) {
        strncpy(buf, "0.0.0.0", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }
    pthread_mutex_lock(&gl.mutex);
    strncpy(buf, gl.local_ip, buf_size - 1);
    buf[buf_size - 1] = '\0';
    pthread_mutex_unlock(&gl.mutex);
}

// Note: Returns pointer to internal buffer - use GBALink_getLocalIPSafe for thread safety
const char* GBALink_getLocalIP(void) {
    // Refresh IP if not in an active session (to avoid returning stale hotspot IP)
    if (gl.mode == GBALINK_OFF) {
        NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    }
    return gl.local_ip;
}

bool GBALink_isUsingHotspot(void) {
    if (!gl.initialized) return false;
    pthread_mutex_lock(&gl.mutex);
    bool using_hotspot = gl.using_hotspot;
    pthread_mutex_unlock(&gl.mutex);
    return using_hotspot;
}

bool GBALink_hasNetworkConnection(void) {
    NET_getLocalIP(gl.local_ip, sizeof(gl.local_ip));
    return NET_hasConnection();
}

void GBALink_update(void) {
    if (!gl.initialized) return;
    pthread_mutex_lock(&gl.mutex);

    // Process pending host connection notification (must run on main thread)
    // The listen thread sets this flag, we process it here to ensure
    // core callbacks are called from the main thread
    if (gl.pending_host_connected) {
        LOG_info("GBALink: HOST update() processing pending_host_connected\n");
        // Memory barrier ensures we see all state from listen thread
        __sync_synchronize();
        gl.pending_host_connected = false;
        pthread_mutex_unlock(&gl.mutex);
        GBALink_notifyConnected(1);  // We are host
        pthread_mutex_lock(&gl.mutex);
    }

    // Check for connection errors via socket error state
    // Optimization: only check every 10 frames to reduce syscall overhead
    // TCP errors are still caught quickly by recv/send operations
    if (gl.tcp_fd >= 0 && ++gl.error_check_counter >= 10) {
        gl.error_check_counter = 0;
        int fd = gl.tcp_fd;  // Cache fd under mutex
        pthread_mutex_unlock(&gl.mutex);

        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            GBALink_disconnect();
            return;
        }
        pthread_mutex_lock(&gl.mutex);
    }

    // Check for connection timeout - disconnect if no packets for too long
    // This detects dead connections that TCP keepalive may miss
    // Only check AFTER handshake is complete (core_registered is set by GBALink_notifyConnected)
    if (gl.tcp_fd >= 0 && gl.state == GBALINK_STATE_CONNECTED && gl.core_registered) {
        struct timeval last_recv = gl.last_packet_received;
        pthread_mutex_unlock(&gl.mutex);

        // Use cached frame time if available, otherwise get fresh time
        const struct timeval* now = get_frame_time();

        long elapsed_ms = (now->tv_sec - last_recv.tv_sec) * 1000 +
                          (now->tv_usec - last_recv.tv_usec) / 1000;

        if (elapsed_ms > GBALINK_CONNECTION_TIMEOUT_MS) {
            GBALink_disconnect();
            return;
        }
    } else {
        pthread_mutex_unlock(&gl.mutex);
    }
}

bool GBALink_getPendingPacket(void** buf, size_t* len, uint16_t* client_id) {
    pthread_mutex_lock(&gl.mutex);
    if (gl.pending_count == 0) {
        pthread_mutex_unlock(&gl.mutex);
        return false;
    }
    ReceivedPacket* pkt = &gl.pending_packets[gl.pending_read_idx];
    *buf = pkt->data;
    *len = pkt->len;
    if (client_id) *client_id = pkt->client_id;
    pthread_mutex_unlock(&gl.mutex);
    return true;
}

void GBALink_consumePendingPacket(void) {
    pthread_mutex_lock(&gl.mutex);
    if (gl.pending_count > 0) {
        gl.pending_read_idx = (gl.pending_read_idx + 1) % MAX_PENDING_PACKETS;
        gl.pending_count--;
    }
    pthread_mutex_unlock(&gl.mutex);
}

// Atomic get-and-consume: reduces mutex cycles in hot path (single lock instead of two)
bool GBALink_popPendingPacket(void** buf, size_t* len, uint16_t* client_id) {
    pthread_mutex_lock(&gl.mutex);
    if (gl.pending_count == 0) {
        pthread_mutex_unlock(&gl.mutex);
        return false;
    }
    ReceivedPacket* pkt = &gl.pending_packets[gl.pending_read_idx];
    *buf = pkt->data;
    *len = pkt->len;
    if (client_id) *client_id = pkt->client_id;
    // Consume immediately
    gl.pending_read_idx = (gl.pending_read_idx + 1) % MAX_PENDING_PACKETS;
    gl.pending_count--;
    pthread_mutex_unlock(&gl.mutex);
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Network Helper Functions
//////////////////////////////////////////////////////////////////////////////

// Forward declaration for drain function
static void drain_receive_buffer(void);

// Send all bytes with retry logic for reliability
// Retries up to 500ms total to handle WiFi latency and buffer pressure
// Critical: RFU protocol breaks if packets are dropped - must deliver all packets
// Returns false only on real errors or extended blocking
// NOTE: This function does NOT hold the mutex - caller must NOT hold mutex either
static bool send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = buf;
    int total_wait_us = 0;
    const int max_wait_us = 2000000;  // 2 seconds - needs to be long enough to survive TCP deadlocks

    while (len > 0) {
        ssize_t sent = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            p += sent;
            len -= sent;
            total_wait_us = 0;  // Reset wait time on successful send
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full - wait briefly and retry
                if (total_wait_us >= max_wait_us) {
                    return false;
                }

                // CRITICAL: While waiting for send buffer to clear, drain our receive buffer
                // This prevents deadlock where both sides are waiting to send but neither is receiving
                // Safe to call without mutex since socket ops are thread-safe
                drain_receive_buffer();

                usleep(1000);  // 1ms
                total_wait_us += 1000;
            } else {
                // Real error (connection closed, broken pipe, etc.)
                return false;
            }
        } else {
            // sent == 0 should not happen with TCP
            return false;
        }
    }
    return true;
}

// Drain receive buffer without holding mutex - used during send retries to prevent deadlock
// This reads from TCP socket to clear the kernel's receive buffer, allowing remote to send more
static void drain_receive_buffer(void) {
    int fd;
    pthread_mutex_lock(&gl.mutex);
    fd = gl.tcp_fd;

    // Compact buffer if needed to make space for draining (uses optimized helper)
    compact_stream_buffer_if_needed(1024);
    size_t space = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;
    pthread_mutex_unlock(&gl.mutex);

    if (fd < 0 || space == 0) return;

    // Check if data is available
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 0};  // Non-blocking

    if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) {
        // Read data into stream buffer (under mutex)
        pthread_mutex_lock(&gl.mutex);
        if (gl.tcp_fd >= 0) {
            space = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;
            if (space > 0) {
                ssize_t ret = recv(gl.tcp_fd, gl.stream_buf + gl.stream_buf_write_idx, space, MSG_DONTWAIT);
                if (ret > 0) {
                    gl.stream_buf_write_idx += ret;
                }
            }
        }
        pthread_mutex_unlock(&gl.mutex);
    }
}

// Send packet - NOTE: caller must hold mutex for shared state, but we release during I/O
static bool send_packet(uint8_t cmd, const void* data, uint16_t size, uint16_t client_id) {
    if (gl.tcp_fd < 0) return false;

    // Get fd before releasing mutex
    int fd = gl.tcp_fd;

    PacketHeader hdr = {
        .cmd = cmd,
        .size = htons(size),
        .client_id = htons(client_id)
    };

    // Release mutex during actual I/O to allow receive processing
    pthread_mutex_unlock(&gl.mutex);

    bool ok = send_all(fd, &hdr, sizeof(hdr));
    if (ok && size > 0 && data) {
        ok = send_all(fd, data, size);
    }

    // Re-acquire mutex before returning
    pthread_mutex_lock(&gl.mutex);

    // Validate fd is still valid (another thread could have disconnected)
    if (gl.tcp_fd < 0 || gl.tcp_fd != fd) {
        return false;
    }

    if (!ok) {
        return false;
    }

    return true;
}

static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms) {
    if (gl.tcp_fd < 0) return false;

    // Calculate available data in buffer
    size_t available = gl.stream_buf_write_idx - gl.stream_buf_read_idx;

    // Try to read more data into our stream buffer (non-blocking)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(gl.tcp_fd, &fds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    // Only try to recv if there's data available (non-blocking check)
    if (select(gl.tcp_fd + 1, &fds, NULL, NULL, &tv) > 0) {
        // Compact buffer if needed (optimized: only when read_idx past halfway)
        compact_stream_buffer_if_needed(1024);
        size_t space_at_end = sizeof(gl.stream_buf) - gl.stream_buf_write_idx;

        if (space_at_end > 0) {
            ssize_t ret = recv(gl.tcp_fd, gl.stream_buf + gl.stream_buf_write_idx, space_at_end, MSG_DONTWAIT);
            if (ret == 0) {
                // Connection closed by remote
                GBALinkMode prev_mode = gl.mode;
                close(gl.tcp_fd);
                gl.tcp_fd = -1;
                gl.core_registered = false;  // Prevent timeout check from firing

                if (prev_mode == GBALINK_CLIENT) {
                    // Client fully disconnects
                    gl.mode = GBALINK_OFF;
                    gl.state = GBALINK_STATE_DISCONNECTED;
                    strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
                    gl.connected_to_hotspot = false;
                    snprintf(gl.status_msg, sizeof(gl.status_msg), "Host disconnected");
                    gl.pending_disconnect_notify = true;  // Defer notification until mutex released
                } else if (prev_mode == GBALINK_HOST) {
                    // Host goes back to waiting and restarts broadcast
                    gl.state = GBALINK_STATE_WAITING;
                    snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
                    gl.pending_disconnect_notify = true;  // Defer notification until mutex released
                    GBALink_restartBroadcast();
                }
                return false;
            }
            if (ret < 0) {
                if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
                    GBALinkMode prev_mode = gl.mode;
                    close(gl.tcp_fd);
                    gl.tcp_fd = -1;
                    gl.core_registered = false;  // Prevent timeout check from firing

                    if (prev_mode == GBALINK_CLIENT) {
                        // Client fully disconnects
                        gl.mode = GBALINK_OFF;
                        gl.state = GBALINK_STATE_DISCONNECTED;
                        strncpy(gl.local_ip, "0.0.0.0", sizeof(gl.local_ip) - 1);
                        gl.connected_to_hotspot = false;
                        snprintf(gl.status_msg, sizeof(gl.status_msg), "Connection lost");
                        gl.pending_disconnect_notify = true;  // Defer notification until mutex released
                    } else if (prev_mode == GBALINK_HOST) {
                        // Host goes back to waiting and restarts broadcast
                        gl.state = GBALINK_STATE_WAITING;
                        snprintf(gl.status_msg, sizeof(gl.status_msg), "Client left, waiting on %s:%d", gl.local_ip, gl.port);
                        gl.pending_disconnect_notify = true;  // Defer notification until mutex released
                        GBALink_restartBroadcast();
                    }
                    return false;
                }
                // EAGAIN/EWOULDBLOCK is ok, just no data right now
            } else {
                gl.stream_buf_write_idx += ret;
                available += ret;
            }
        }
    }

    // Check if we have a complete header
    if (available < sizeof(PacketHeader)) {
        return false;  // Need more data
    }

    // Parse header from buffer
    PacketHeader* buf_hdr = (PacketHeader*)(gl.stream_buf + gl.stream_buf_read_idx);
    hdr->cmd = buf_hdr->cmd;
    hdr->size = ntohs(buf_hdr->size);
    hdr->client_id = ntohs(buf_hdr->client_id);

    // Validate size - explicit bounds check for safety
    // Check both against max_size (caller's buffer) and RECV_BUFFER_SIZE (our buffer)
    if (hdr->size > max_size || hdr->size > RECV_BUFFER_SIZE) {
        // Invalid packet size - protocol error, reset buffer
        gl.stream_buf_read_idx = 0;
        gl.stream_buf_write_idx = 0;
        return false;
    }

    // Check if we have complete packet (header + payload)
    size_t total_size = sizeof(PacketHeader) + hdr->size;
    if (available < total_size) {
        return false;  // Need more data
    }

    // Copy payload to output (bounds already validated above)
    if (hdr->size > 0 && data) {
        memcpy(data, gl.stream_buf + gl.stream_buf_read_idx + sizeof(PacketHeader), hdr->size);
    }

    // Advance read index instead of memmove - O(1) instead of O(n)
    gl.stream_buf_read_idx += total_size;

    // If buffer is now empty, reset indices to avoid accumulating offset
    if (gl.stream_buf_read_idx == gl.stream_buf_write_idx) {
        gl.stream_buf_read_idx = 0;
        gl.stream_buf_write_idx = 0;
    }

    // Update last packet received timestamp - use cached time if available
    const struct timeval* now = get_frame_time();
    gl.last_packet_received = *now;

    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Core Netpacket Bridging
//////////////////////////////////////////////////////////////////////////////

// Maximum packets to deliver per frame - matches minarch's constant
#define GBALINK_MAX_PACKETS_PER_FRAME 64

// Set core netpacket callbacks (called by minarch when core registers)
void GBALink_setCoreCallbacks(const struct retro_netpacket_callback* callbacks) {
    if (callbacks) {
        gl.core_callbacks = *callbacks;
        gl.has_core_callbacks = true;
        gl.has_netpacket_support = true;
        LOG_info("GBALink: Core registered netpacket callbacks (start=%p receive=%p)\n",
                 (void*)callbacks->start, (void*)callbacks->receive);
    } else {
        memset(&gl.core_callbacks, 0, sizeof(gl.core_callbacks));
        gl.has_core_callbacks = false;
        gl.has_netpacket_support = false;
        LOG_info("GBALink: Core unregistered netpacket callbacks\n");
    }
}

// Send function provided to core - bridges to gbalink network
static void gbalink_netpacket_send(int flags, const void* buf, size_t len, uint16_t client_id) {
    if (gl.netpacket_active) {
        GBALink_sendPacket(flags, buf, len, client_id);
    }
}

// Poll receive function provided to core
static void gbalink_netpacket_poll_receive(void) {
    if (!gl.netpacket_active) return;
    GBALink_pollReceive();
}

// Start netpacket session - called when gbalink connects
void GBALink_notifyConnected(int is_host) {
    if (!gl.has_core_callbacks || gl.netpacket_active) {
        return;
    }

    // Call core's start callback with our bridge functions
    if (gl.core_callbacks.start) {
        uint16_t client_id = is_host ? 0 : 1;  // 0 = host, 1 = client
        gl.local_client_id = client_id;
        gl.remote_client_id = is_host ? 1 : 0;
        gl.core_callbacks.start(client_id, gbalink_netpacket_send, gbalink_netpacket_poll_receive);
        gl.netpacket_active = true;

        // Register for timeout detection
        GBALink_onNetpacketStart(client_id, NULL, NULL);
    }

    // Notify core that remote player connected
    if (gl.core_callbacks.connected) {
        gl.core_callbacks.connected(gl.remote_client_id);
    }
}

// Stop netpacket session - called when gbalink disconnects
void GBALink_notifyDisconnected(void) {
    if (!gl.netpacket_active) return;

    // Notify core that remote player disconnected
    if (gl.core_callbacks.disconnected) {
        gl.core_callbacks.disconnected(gl.remote_client_id);
    }

    // Call core's stop callback
    if (gl.core_callbacks.stop) {
        gl.core_callbacks.stop();
    }

    // Unregister from timeout tracking
    GBALink_onNetpacketStop();

    gl.netpacket_active = false;
}

// Check if netpacket bridging is active
bool GBALink_isNetpacketActive(void) {
    return gl.netpacket_active;
}

// Poll network and deliver packets to core (call each frame before core.run())
void GBALink_pollAndDeliverPackets(void) {
    if (!gl.netpacket_active) return;

    // Poll for incoming TCP data
    GBALink_pollReceive();

    // Deliver pending packets to core
    // Use atomic pop to reduce mutex cycles (single lock instead of get+consume)
    void* pkt_buf;
    size_t pkt_len;

    int packets_delivered = 0;
    while (packets_delivered < GBALINK_MAX_PACKETS_PER_FRAME &&
           GBALink_popPendingPacket(&pkt_buf, &pkt_len, NULL)) {
        // In direct 2-player TCP, any received packet is from the remote peer
        if (gl.core_callbacks.receive) {
            gl.core_callbacks.receive(pkt_buf, pkt_len, gl.remote_client_id);
        }
        packets_delivered++;
    }
}

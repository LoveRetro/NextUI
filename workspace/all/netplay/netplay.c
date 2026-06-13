/*
 * NextUI Netplay Module
 * Simplified implementation based on RetroArch netplay concepts
 *
 * Key design:
 * - Lockstep synchronization: both devices must have same inputs before advancing
 * - Frame buffer: circular buffer storing input history
 * - Host = Player 1, Client = Player 2 (always)
 * - Both devices run identical emulation with identical inputs
 */

#define _GNU_SOURCE  // For strcasestr

#include "netplay.h"
#include "netplay_helper.h"  // For stopHotspotAndRestoreWiFiAsync, netplay_connected_to_hotspot
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

// Protocol constants (internal)
#define NP_PROTOCOL_MAGIC   0x4E585550  // "NXUP" - NextUI Protocol
#define NP_DISCOVERY_QUERY  0x4E584451  // "NXDQ" - NextUI Discovery Query
#define NP_DISCOVERY_RESP   0x4E584452  // "NXDR" - NextUI Discovery Response

// Optimization: Discovery broadcast interval (microseconds)
#define DISCOVERY_BROADCAST_INTERVAL_US 500000  // 500ms

// Network commands
enum {
    CMD_INPUT      = 0x01,  // Input data for a frame
    CMD_STATE_REQ  = 0x02,  // Request state transfer
    CMD_STATE_HDR  = 0x03,  // State header (size)
    CMD_STATE_DATA = 0x04,  // State data chunk
    CMD_STATE_ACK  = 0x05,  // State received OK
    CMD_PING       = 0x06,
    CMD_PONG       = 0x07,
    CMD_DISCONNECT = 0x08,
    CMD_READY      = 0x09,  // Ready to play
    CMD_PAUSE      = 0x0A,  // Player paused (menu opened)
    CMD_RESUME     = 0x0B,  // Player resumed (menu closed)
    CMD_KEEPALIVE  = 0x0C,  // Keepalive during stall to prevent timeout
};

// Frame input entry
typedef struct {
    uint32_t frame;
    uint16_t p1_input;  // Host input (always Player 1)
    uint16_t p2_input;  // Client input (always Player 2)
    bool have_p1;
    bool have_p2;
} FrameInput;

// Packet header
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint32_t frame;
    uint16_t size;
} PacketHeader;

// Input packet
typedef struct __attribute__((packed)) {
    uint16_t input;
} InputPacket;

// Main netplay state
static struct {
    NetplayMode mode;
    NetplayState state;

    // Sockets
    int tcp_fd;         // Main TCP connection
    int listen_fd;      // Server listen socket
    int udp_fd;         // Discovery UDP socket

    // Connection info
    char local_ip[16];
    char remote_ip[16];
    uint16_t port;

    // Game info
    char game_name[NETPLAY_MAX_GAME_NAME];
    uint32_t game_crc;

    // Frame synchronization
    uint32_t self_frame;        // Our current frame
    uint32_t run_frame;         // Frame we're executing
    uint32_t other_frame;       // Last frame with complete input

    // Circular frame buffer
    FrameInput frame_buffer[NETPLAY_FRAME_BUFFER_SIZE];

    // Local input for current frame
    uint16_t local_input;

    // State sync flags
    bool needs_state_sync;
    bool state_sync_complete;

    // Discovery
    NetplayHostInfo discovered_hosts[NETPLAY_MAX_HOSTS];
    int num_hosts;
    bool discovery_active;

    // Threading
    pthread_t listen_thread;
    pthread_mutex_t mutex;
    volatile bool running;

    // Status
    char status_msg[128];
    int stall_frames;

    // Optimization: Cached audio silence state (updated per frame)
    volatile bool audio_should_silence;

    // Hotspot mode
    bool using_hotspot;

    // Pause state (for menu)
    bool local_paused;   // We have paused (menu open)
    bool remote_paused;  // Remote player has paused

    // Initialization flag
    bool initialized;

} np = {0};

// Forward declarations
static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size);
static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms);
static void* listen_thread_func(void* arg);
static FrameInput* get_frame_slot(uint32_t frame);
static void init_frame_buffer(void);

//////////////////////////////////////////////////////////////////////////////
// Initialization
//////////////////////////////////////////////////////////////////////////////

void Netplay_init(void) {
    if (np.initialized) return;

    memset(&np, 0, sizeof(np));
    np.mode = NETPLAY_OFF;
    np.state = NETPLAY_STATE_IDLE;
    np.tcp_fd = -1;
    np.listen_fd = -1;
    np.udp_fd = -1;
    np.port = NETPLAY_DEFAULT_PORT;
    pthread_mutex_init(&np.mutex, NULL);
    NET_getLocalIP(np.local_ip, sizeof(np.local_ip));
    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay ready");
    np.initialized = true;
}

void Netplay_quit(void) {
    if (!np.initialized) return;

    // Capture hotspot state before cleanup
    bool was_host = (np.mode == NETPLAY_HOST);
    bool needs_hotspot_cleanup = np.using_hotspot || netplay_connected_to_hotspot;

    Netplay_disconnect();
    Netplay_stopHostFast();
    Netplay_stopDiscovery();

    // Handle hotspot cleanup asynchronously
    if (needs_hotspot_cleanup) {
        stopHotspotAndRestoreWiFiAsync(was_host);
        netplay_connected_to_hotspot = 0;
    }

    pthread_mutex_destroy(&np.mutex);
    np.initialized = false;
}

bool Netplay_checkCoreSupport(const char* core_name) {
    // These cores have been tested and work with frame-synchronized netplay
    // core_name is derived from the .so filename (e.g., "fbneo" from "fbneo_libretro.so")
    if (strcasecmp(core_name, "fbneo") == 0 ||
        strcasecmp(core_name, "fceumm") == 0 ||
        strcasecmp(core_name, "snes9x") == 0 ||
        strcasecmp(core_name, "mednafen_supafaust") == 0 ||
        strcasecmp(core_name, "picodrive") == 0 ||
        strcasecmp(core_name, "pcsx_rearmed") == 0) {
        return true;
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
// Helper Functions (extracted for code reuse)
//////////////////////////////////////////////////////////////////////////////

static FrameInput* get_frame_slot(uint32_t frame) {
    return &np.frame_buffer[frame & NETPLAY_FRAME_MASK];
}

static void init_frame_slot(uint32_t frame) {
    FrameInput* slot = get_frame_slot(frame);
    slot->frame = frame;
    slot->p1_input = 0;
    slot->p2_input = 0;
    slot->have_p1 = false;
    slot->have_p2 = false;
}

// Optimization: Extracted duplicate frame buffer initialization
static void init_frame_buffer(void) {
    for (int i = 0; i < NETPLAY_FRAME_BUFFER_SIZE; i++) {
        init_frame_slot(i);
    }
}

//////////////////////////////////////////////////////////////////////////////
// Host Mode
//////////////////////////////////////////////////////////////////////////////

int Netplay_startHost(const char* game_name, uint32_t game_crc, const char* hotspot_ip) {
    Netplay_init();  // Lazy init
    if (np.mode != NETPLAY_OFF) {
        return -1;
    }

    // Set up IP based on mode
    if (hotspot_ip) {
        np.using_hotspot = true;
        strncpy(np.local_ip, hotspot_ip, sizeof(np.local_ip) - 1);
        np.local_ip[sizeof(np.local_ip) - 1] = '\0';
    }

    // Create TCP listen socket using shared utility
    np.listen_fd = NET_createListenSocket(np.port, np.status_msg, sizeof(np.status_msg));
    if (np.listen_fd < 0) {
        if (hotspot_ip) {
            np.using_hotspot = false;
        }
        return -1;
    }

    // Create UDP socket for discovery broadcasts
    np.udp_fd = NET_createBroadcastSocket();
    if (np.udp_fd < 0) {
        close(np.listen_fd);
        np.listen_fd = -1;
        if (hotspot_ip) {
            np.using_hotspot = false;
        }
        snprintf(np.status_msg, sizeof(np.status_msg), "Failed to create broadcast socket");
        return -1;
    }

    strncpy(np.game_name, game_name, NETPLAY_MAX_GAME_NAME - 1);
    np.game_crc = game_crc;

    // Start listen thread
    np.running = true;
    pthread_create(&np.listen_thread, NULL, listen_thread_func, NULL);

    np.mode = NETPLAY_HOST;
    np.state = NETPLAY_STATE_WAITING;
    np.needs_state_sync = true;

    snprintf(np.status_msg, sizeof(np.status_msg), "Hosting on %s:%d", np.local_ip, np.port);
    return 0;
}

void Netplay_stopBroadcast(void) {
    // Close UDP socket - no longer needed after connection
    if (np.udp_fd >= 0) {
        close(np.udp_fd);
        np.udp_fd = -1;
    }
}

// Restart UDP broadcast when going back to waiting state
// Called when client disconnects but host wants to accept new clients
static void Netplay_restartBroadcast(void) {
    if (np.udp_fd >= 0) return;  // Already running
    if (np.mode != NETPLAY_HOST) return;  // Only for host

    np.udp_fd = NET_createBroadcastSocket();
    if (np.udp_fd < 0) {
        snprintf(np.status_msg, sizeof(np.status_msg), "Failed to restart broadcast");
    }
}

// Internal helper - stops host with optional hotspot cleanup
static int Netplay_stopHostInternal(bool skip_hotspot_cleanup) {
    if (np.mode != NETPLAY_HOST) return -1;

    np.running = false;

    // Wake up listen thread by closing listen socket (causes select to return)
    // This is safer than pthread_cancel which may leave resources inconsistent
    if (np.listen_fd >= 0) {
        shutdown(np.listen_fd, SHUT_RDWR);
    }

    if (np.listen_thread) {
        pthread_join(np.listen_thread, NULL);
        np.listen_thread = 0;
    }

    if (np.listen_fd >= 0) {
        close(np.listen_fd);
        np.listen_fd = -1;
    }

    Netplay_stopBroadcast();
    Netplay_disconnect();

    // Stop hotspot if it was started
    if (np.using_hotspot) {
        if (!skip_hotspot_cleanup) {
#ifdef HAS_WIFIMG
            WIFI_direct_stopHotspot();
#endif
        }
        np.using_hotspot = false;
    }

    np.mode = NETPLAY_OFF;
    np.state = NETPLAY_STATE_IDLE;
    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay ready");
    return 0;
}

int Netplay_stopHost(void) {
    return Netplay_stopHostInternal(false);
}

int Netplay_stopHostFast(void) {
    return Netplay_stopHostInternal(true);
}

static void* listen_thread_func(void* arg) {
    (void)arg;

    // Use shared broadcast timer for rate limiting
    NET_BroadcastTimer broadcast_timer;
    NET_initBroadcastTimer(&broadcast_timer, DISCOVERY_BROADCAST_INTERVAL_US);

    while (np.running && np.listen_fd >= 0) {
        // Check state under mutex protection to avoid race conditions
        pthread_mutex_lock(&np.mutex);
        bool is_waiting = (np.state == NETPLAY_STATE_WAITING);
        int udp_fd = np.udp_fd;
        pthread_mutex_unlock(&np.mutex);

        // Rate-limited discovery broadcast using shared timer
        if (udp_fd >= 0 && is_waiting) {
            if (NET_shouldBroadcast(&broadcast_timer)) {
                NET_sendDiscoveryBroadcast(udp_fd, NP_DISCOVERY_RESP, NETPLAY_PROTOCOL_VERSION,
                                           np.game_crc, np.port, NETPLAY_DISCOVERY_PORT,
                                           np.game_name, NULL);  // Netplay doesn't use link_mode
            }
        }

        // Check for incoming connection (only accept when waiting)
        if (is_waiting) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(np.listen_fd, &fds);

            struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms timeout
            if (select(np.listen_fd + 1, &fds, NULL, NULL, &tv) > 0) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int fd = accept(np.listen_fd, (struct sockaddr*)&client_addr, &len);
                if (fd >= 0) {
                    pthread_mutex_lock(&np.mutex);

                    // Double-check we're still waiting (state could have changed)
                    if (np.state != NETPLAY_STATE_WAITING) {
                        close(fd);
                        pthread_mutex_unlock(&np.mutex);
                        continue;
                    }

                    // Configure TCP socket using shared utility (default: 64KB buffers)
                    NET_configureTCPSocket(fd, NULL);

                    np.tcp_fd = fd;
                    inet_ntop(AF_INET, &client_addr.sin_addr, np.remote_ip, sizeof(np.remote_ip));

                    np.state = NETPLAY_STATE_SYNCING;
                    np.needs_state_sync = true;
                    np.self_frame = 0;
                    np.run_frame = 0;
                    np.other_frame = 0;

                    init_frame_buffer();

                    snprintf(np.status_msg, sizeof(np.status_msg), "Client connected: %s", np.remote_ip);
                    pthread_mutex_unlock(&np.mutex);
                }
            }
        } else {
            // Not waiting, just sleep briefly to avoid busy loop
            usleep(50000);  // 50ms (reduced from 100ms)
        }
    }

    return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Client Mode
//////////////////////////////////////////////////////////////////////////////

int Netplay_connectToHost(const char* ip, uint16_t port) {
    Netplay_init();  // Lazy init
    if (np.mode != NETPLAY_OFF) {
        return -1;
    }

    np.tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (np.tcp_fd < 0) {
        snprintf(np.status_msg, sizeof(np.status_msg), "Socket creation failed");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(np.tcp_fd);
        np.tcp_fd = -1;
        snprintf(np.status_msg, sizeof(np.status_msg), "Invalid IP address");
        return -1;
    }

    np.state = NETPLAY_STATE_CONNECTING;
    snprintf(np.status_msg, sizeof(np.status_msg), "Connecting to %s:%d...", ip, port);

    // Connect with timeout
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(np.tcp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(np.tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(np.tcp_fd);
        np.tcp_fd = -1;
        np.state = NETPLAY_STATE_ERROR;
        snprintf(np.status_msg, sizeof(np.status_msg), "Connection failed");
        return -1;
    }

    // Configure TCP socket using shared utility (default: 64KB buffers)
    NET_configureTCPSocket(np.tcp_fd, NULL);

    strncpy(np.remote_ip, ip, sizeof(np.remote_ip) - 1);
    np.port = port;
    np.mode = NETPLAY_CLIENT;
    np.state = NETPLAY_STATE_SYNCING;
    np.needs_state_sync = true;

    np.self_frame = 0;
    np.run_frame = 0;
    np.other_frame = 0;

    init_frame_buffer();

    snprintf(np.status_msg, sizeof(np.status_msg), "Connected to %s", ip);
    return 0;
}

void Netplay_disconnect(void) {
    if (np.tcp_fd >= 0) {
        send_packet(CMD_DISCONNECT, 0, NULL, 0);
        close(np.tcp_fd);
        np.tcp_fd = -1;
    }

    // Update cached audio state
    np.audio_should_silence = false;

    // Reset pause state
    np.local_paused = false;
    np.remote_paused = false;

    if (np.mode == NETPLAY_CLIENT) {
        np.mode = NETPLAY_OFF;
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Disconnected");
    } else if (np.mode == NETPLAY_HOST) {
        // Host stays in host mode, reset to waiting for new client
        np.state = NETPLAY_STATE_WAITING;
        np.needs_state_sync = true;
        np.stall_frames = 0;
        snprintf(np.status_msg, sizeof(np.status_msg), "Client left, waiting on %s:%d", np.local_ip, np.port);
    } else {
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Disconnected");
    }
}

//////////////////////////////////////////////////////////////////////////////
// Discovery
//////////////////////////////////////////////////////////////////////////////

int Netplay_startDiscovery(void) {
    if (np.discovery_active) return 0;

    np.udp_fd = NET_createDiscoveryListenSocket(NETPLAY_DISCOVERY_PORT);
    if (np.udp_fd < 0) {
        snprintf(np.status_msg, sizeof(np.status_msg), "Failed to start discovery");
        return -1;
    }

    np.num_hosts = 0;
    np.discovery_active = true;
    return 0;
}

void Netplay_stopDiscovery(void) {
    if (!np.discovery_active) return;

    if (np.udp_fd >= 0 && np.mode == NETPLAY_OFF) {
        close(np.udp_fd);
        np.udp_fd = -1;
    }

    np.discovery_active = false;
}

int Netplay_getDiscoveredHosts(NetplayHostInfo* hosts, int max_hosts) {
    if (!np.discovery_active || np.udp_fd < 0) return 0;

    // Poll for discovery responses using shared function
    // NetplayHostInfo and NET_HostInfo have identical layouts
    NET_receiveDiscoveryResponses(np.udp_fd, NP_DISCOVERY_RESP,
                                   (NET_HostInfo*)np.discovered_hosts, &np.num_hosts,
                                   NETPLAY_MAX_HOSTS);

    int count = (np.num_hosts < max_hosts) ? np.num_hosts : max_hosts;
    memcpy(hosts, np.discovered_hosts, count * sizeof(NetplayHostInfo));
    return count;
}

//////////////////////////////////////////////////////////////////////////////
// Frame Synchronization (Core Netplay Logic)
//////////////////////////////////////////////////////////////////////////////

bool Netplay_preFrame(void) {
    pthread_mutex_lock(&np.mutex);

    // Check connection under mutex to avoid TOCTOU race
    if (np.tcp_fd < 0 ||
        (np.state != NETPLAY_STATE_SYNCING &&
         np.state != NETPLAY_STATE_PLAYING &&
         np.state != NETPLAY_STATE_STALLED &&
         np.state != NETPLAY_STATE_PAUSED)) {
        pthread_mutex_unlock(&np.mutex);
        return true;
    }

    // Get slot for execution (current run frame)
    FrameInput* run_slot = get_frame_slot(np.run_frame);

    // Get slot for input sending (ahead by latency frames)
    FrameInput* input_slot = get_frame_slot(np.self_frame);
    if (input_slot->frame != np.self_frame) {
        init_frame_slot(np.self_frame);
        input_slot->frame = np.self_frame;
    }

    // Store and send our input for the FUTURE frame (np.self_frame)
    if (np.mode == NETPLAY_HOST) {
        if (!input_slot->have_p1) {
            input_slot->p1_input = np.local_input;
            input_slot->have_p1 = true;
            InputPacket pkt = { .input = htons(np.local_input) };
            send_packet(CMD_INPUT, np.self_frame, &pkt, sizeof(pkt));
        }
    } else {
        if (!input_slot->have_p2) {
            input_slot->p2_input = np.local_input;
            input_slot->have_p2 = true;
            InputPacket pkt = { .input = htons(np.local_input) };
            send_packet(CMD_INPUT, np.self_frame, &pkt, sizeof(pkt));
        }
    }

    // Try to receive remote input - always process available packets
    int timeout_ms = 16;   // ~1 frame at 60fps
    int max_attempts = 10; // ~160ms total
    int attempts = 0;

    while (attempts < max_attempts) {
        // Check if we already have both inputs for the run frame
        run_slot = get_frame_slot(np.run_frame);
        if (run_slot->have_p1 && run_slot->have_p2) {
            break;  // Got both inputs, proceed
        }

        // Release lock during blocking network operation
        pthread_mutex_unlock(&np.mutex);

        // Try to receive remote input
        PacketHeader hdr;
        InputPacket remote_pkt;
        bool received = recv_packet(&hdr, &remote_pkt, sizeof(remote_pkt), timeout_ms);

        // Re-acquire lock for frame buffer access
        pthread_mutex_lock(&np.mutex);

        // Check if disconnected during recv
        if (np.state == NETPLAY_STATE_DISCONNECTED) {
            np.audio_should_silence = false;
            pthread_mutex_unlock(&np.mutex);
            return false;
        }

        if (received) {
            if (hdr.cmd == CMD_INPUT) {
                FrameInput* remote_slot = get_frame_slot(hdr.frame);
                uint16_t remote_input = ntohs(remote_pkt.input);

                // Store remote input in appropriate slot
                if (np.mode == NETPLAY_HOST) {
                    remote_slot->p2_input = remote_input;
                    remote_slot->have_p2 = true;
                } else {
                    remote_slot->p1_input = remote_input;
                    remote_slot->have_p1 = true;
                }
            } else if (hdr.cmd == CMD_DISCONNECT) {
                // Close TCP connection
                close(np.tcp_fd);
                np.tcp_fd = -1;
                np.audio_should_silence = false;

                // For host, go back to waiting and restart broadcast
                if (np.mode == NETPLAY_HOST) {
                    np.state = NETPLAY_STATE_WAITING;
                    np.needs_state_sync = true;
                    np.stall_frames = 0;
                    Netplay_restartBroadcast();
                    snprintf(np.status_msg, sizeof(np.status_msg), "Client left, waiting on %s:%d", np.local_ip, np.port);
                } else {
                    np.state = NETPLAY_STATE_DISCONNECTED;
                    snprintf(np.status_msg, sizeof(np.status_msg), "Host disconnected");
                }
                pthread_mutex_unlock(&np.mutex);
                return false;
            } else if (hdr.cmd == CMD_PAUSE) {
                np.remote_paused = true;
                np.state = NETPLAY_STATE_PAUSED;
                snprintf(np.status_msg, sizeof(np.status_msg), "Remote player paused");
            } else if (hdr.cmd == CMD_RESUME) {
                np.remote_paused = false;
                if (!np.local_paused) {
                    np.state = NETPLAY_STATE_PLAYING;
                    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
                }
            } else if (hdr.cmd == CMD_KEEPALIVE) {
                // Keepalive received - connection is alive, reset stall counter
                // This prevents timeout during legitimate delays (save operations, etc.)
            }
        }
        attempts++;
    }

    // Final check - do we have both inputs for run_frame?
    run_slot = get_frame_slot(np.run_frame);
    if (!run_slot->have_p1 || !run_slot->have_p2) {
        np.stall_frames++;

        // Send keepalive during stall to prevent remote from timing out
        if (np.stall_frames % NETPLAY_KEEPALIVE_INTERVAL_FRAMES == 0) {
            send_packet(CMD_KEEPALIVE, np.self_frame, NULL, 0);
        }

        // Skip timeout when either player is paused (menu open)
        if (!np.local_paused && !np.remote_paused) {
            if (np.stall_frames > NETPLAY_STALL_TIMEOUT_FRAMES) {
                snprintf(np.status_msg, sizeof(np.status_msg), "Connection timeout");
                np.state = NETPLAY_STATE_DISCONNECTED;
                np.audio_should_silence = false;
                pthread_mutex_unlock(&np.mutex);
                return false;
            } else if (np.stall_frames > NETPLAY_STALL_WARNING_FRAMES) {
                // Show countdown warning to user
                int remaining = (NETPLAY_STALL_TIMEOUT_FRAMES - np.stall_frames) / 60;
                snprintf(np.status_msg, sizeof(np.status_msg), "Waiting... (%ds)", remaining);
            }
        }
        np.state = NETPLAY_STATE_STALLED;
        np.audio_should_silence = true;
        pthread_mutex_unlock(&np.mutex);
        return false;
    }

    np.stall_frames = 0;
    np.audio_should_silence = false;
    np.state = NETPLAY_STATE_PLAYING;
    pthread_mutex_unlock(&np.mutex);
    return true;
}

uint16_t Netplay_getInputState(unsigned port) {
    if (!Netplay_isConnected()) return 0;

    pthread_mutex_lock(&np.mutex);
    FrameInput* slot = get_frame_slot(np.run_frame);
    uint16_t input = (port == 0) ? slot->p1_input : slot->p2_input;
    pthread_mutex_unlock(&np.mutex);

    return input;
}

uint32_t Netplay_getPlayerButtons(unsigned port, uint32_t local_buttons) {
    // When netplay active, inputs come from the synchronized frame buffer
    // Host = Player 1, Client = Player 2 (always)
    // Both devices see identical inputs for same frame
    if (np.mode != NETPLAY_OFF && Netplay_isConnected()) {
        return Netplay_getInputState(port);
    }
    // Local play - only P1 has input
    return (port == 0) ? local_buttons : 0;
}

void Netplay_setLocalInput(uint16_t input) {
    np.local_input = input;
}

void Netplay_postFrame(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.run_frame++;
    np.self_frame++;
    pthread_mutex_unlock(&np.mutex);
}

bool Netplay_shouldStall(void) {
    return np.state == NETPLAY_STATE_STALLED;
}

// Optimization: Uses cached value instead of checking state each call
bool Netplay_shouldSilenceAudio(void) {
    return np.audio_should_silence;
}

//////////////////////////////////////////////////////////////////////////////
// State Synchronization
//////////////////////////////////////////////////////////////////////////////

int Netplay_sendState(const void* data, size_t size) {
    if (!Netplay_isConnected() || !data || size == 0) return -1;

    // Send state header
    uint32_t state_size = (uint32_t)size;
    state_size = htonl(state_size);
    if (!send_packet(CMD_STATE_HDR, 0, &state_size, sizeof(state_size))) {
        return -1;
    }

    // Send state data in chunks
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        size_t chunk = (remaining > 4096) ? 4096 : remaining;
        ssize_t sent = send(np.tcp_fd, ptr, chunk, MSG_NOSIGNAL);
        if (sent <= 0) {
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    // Wait for client ACK
    PacketHeader hdr;
    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_STATE_ACK) {
        return -1;
    }

    // Send READY signal
    if (!send_packet(CMD_READY, 0, NULL, 0)) {
        return -1;
    }

    return 0;
}

int Netplay_receiveState(void* data, size_t size) {
    if (!Netplay_isConnected() || !data || size == 0) return -1;

    // Receive state header
    PacketHeader hdr;
    uint32_t state_size;

    if (!recv_packet(&hdr, &state_size, sizeof(state_size), 10000) ||
        hdr.cmd != CMD_STATE_HDR) {
        return -1;
    }

    state_size = ntohl(state_size);

    if (state_size != size) {
        snprintf(np.status_msg, sizeof(np.status_msg),
                 "State size mismatch: %u vs %zu", state_size, size);
        return -1;
    }

    // Receive state data
    uint8_t* ptr = (uint8_t*)data;
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t received = recv(np.tcp_fd, ptr, remaining, 0);
        if (received <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        ptr += received;
        remaining -= received;
    }

    // Send ACK to host
    if (!send_packet(CMD_STATE_ACK, 0, NULL, 0)) {
        return -1;
    }

    // Wait for READY signal from host
    if (!recv_packet(&hdr, NULL, 0, 10000) || hdr.cmd != CMD_READY) {
        return -1;
    }

    return 0;
}

bool Netplay_needsStateSync(void) {
    return np.needs_state_sync && np.state == NETPLAY_STATE_SYNCING;
}

void Netplay_completeStateSync(void) {
    pthread_mutex_lock(&np.mutex);
    np.needs_state_sync = false;
    np.state_sync_complete = true;
    np.state = NETPLAY_STATE_PLAYING;

    // Pre-fill latency buffer frames with neutral input
    for (int i = 0; i < NETPLAY_INPUT_LATENCY_FRAMES; i++) {
        FrameInput* slot = get_frame_slot(i);
        slot->frame = i;
        slot->p1_input = 0;
        slot->p2_input = 0;
        slot->have_p1 = true;
        slot->have_p2 = true;
    }

    np.run_frame = 0;
    np.self_frame = NETPLAY_INPUT_LATENCY_FRAMES;
    np.stall_frames = 0;
    np.audio_should_silence = false;

    snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
    pthread_mutex_unlock(&np.mutex);
}

//////////////////////////////////////////////////////////////////////////////
// Status Functions
//////////////////////////////////////////////////////////////////////////////

NetplayMode Netplay_getMode(void) { return np.mode; }
NetplayState Netplay_getState(void) { return np.state; }
bool Netplay_isUsingHotspot(void) { return np.using_hotspot; }

bool Netplay_isConnected(void) {
    return np.tcp_fd >= 0 &&
           (np.state == NETPLAY_STATE_SYNCING ||
            np.state == NETPLAY_STATE_PLAYING ||
            np.state == NETPLAY_STATE_STALLED ||
            np.state == NETPLAY_STATE_PAUSED);
}

bool Netplay_isActive(void) {
    return np.state == NETPLAY_STATE_PLAYING;
}

const char* Netplay_getStatusMessage(void) { return np.status_msg; }

const char* Netplay_getLocalIP(void) {
    // Refresh IP if not in an active session (to avoid returning stale hotspot IP)
    if (np.mode == NETPLAY_OFF) {
        NET_getLocalIP(np.local_ip, sizeof(np.local_ip));
    }
    return np.local_ip;
}

bool Netplay_hasNetworkConnection(void) {
    NET_getLocalIP(np.local_ip, sizeof(np.local_ip));
    return NET_hasConnection();
}

//////////////////////////////////////////////////////////////////////////////
// Pause/Resume for Menu
//////////////////////////////////////////////////////////////////////////////

void Netplay_pause(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.local_paused = true;
    send_packet(CMD_PAUSE, 0, NULL, 0);
    np.state = NETPLAY_STATE_PAUSED;
    snprintf(np.status_msg, sizeof(np.status_msg), "Paused");
    pthread_mutex_unlock(&np.mutex);
}

void Netplay_resume(void) {
    if (!Netplay_isConnected()) return;

    pthread_mutex_lock(&np.mutex);
    np.local_paused = false;
    send_packet(CMD_RESUME, 0, NULL, 0);

    // Only resume to PLAYING if remote is also not paused
    if (!np.remote_paused) {
        np.state = NETPLAY_STATE_PLAYING;
        np.stall_frames = 0;
        snprintf(np.status_msg, sizeof(np.status_msg), "Netplay active");
    } else {
        snprintf(np.status_msg, sizeof(np.status_msg), "Waiting for remote...");
    }
    pthread_mutex_unlock(&np.mutex);
}

void Netplay_pollWhilePaused(void) {
    if (!Netplay_isConnected()) return;

    // Just check if connection is still alive, don't consume any packets
    // Input packets will be processed by Netplay_preFrame() when we resume

    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(np.tcp_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        pthread_mutex_lock(&np.mutex);
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Connection lost");
        close(np.tcp_fd);
        np.tcp_fd = -1;
        pthread_mutex_unlock(&np.mutex);
    }
}

//////////////////////////////////////////////////////////////////////////////
// Main Loop Update
//////////////////////////////////////////////////////////////////////////////

int Netplay_update(uint16_t local_input,
                   Netplay_SerializeSizeFn serialize_size_fn,
                   Netplay_SerializeFn serialize_fn,
                   Netplay_UnserializeFn unserialize_fn) {
    // Handle state sync when connection is established
    if (Netplay_needsStateSync()) {
        if (!serialize_size_fn || !serialize_fn || !unserialize_fn) {
            Netplay_disconnect();
            return 1;  // Run frame normally after disconnect
        }

        size_t state_size = serialize_size_fn();
        bool sync_success = false;

        if (state_size > 0) {
            void* state_data = malloc(state_size);
            if (state_data) {
                if (np.mode == NETPLAY_HOST) {
                    // Host sends current state to client
                    if (serialize_fn(state_data, state_size)) {
                        if (Netplay_sendState(state_data, state_size) == 0) {
                            sync_success = true;
                        }
                    }
                } else {
                    // Client receives state from host
                    if (Netplay_receiveState(state_data, state_size) == 0) {
                        if (unserialize_fn(state_data, state_size)) {
                            sync_success = true;
                        }
                    }
                }
                free(state_data);
            }
        }

        if (sync_success) {
            Netplay_completeStateSync();
        } else {
            Netplay_disconnect();
        }
        return 0;  // Skip this frame
    }

    // Frame synchronization (when playing or recovering from stall)
    if (Netplay_isActive() || Netplay_shouldStall()) {
        Netplay_setLocalInput(local_input);

        if (!Netplay_preFrame()) {
            // Check if we got disconnected
            if (np.state == NETPLAY_STATE_DISCONNECTED) {
                Netplay_disconnect();  // Clean up netplay state
                return 1;  // Continue to run the game normally
            }
            // Stalled - don't run this frame
            return 0;
        }
    }

    return 1;  // Run frame
}

bool Netplay_isPaused(void) {
    return np.local_paused || np.remote_paused;
}

//////////////////////////////////////////////////////////////////////////////
// Network Helper Functions
//////////////////////////////////////////////////////////////////////////////

static bool send_packet(uint8_t cmd, uint32_t frame, const void* data, uint16_t size) {
    if (np.tcp_fd < 0) return false;

    PacketHeader hdr = {
        .cmd = cmd,
        .frame = htonl(frame),
        .size = htons(size)
    };

    if (send(np.tcp_fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) != sizeof(hdr)) {
        return false;
    }

    if (size > 0 && data) {
        if (send(np.tcp_fd, data, size, MSG_NOSIGNAL) != size) {
            return false;
        }
    }

    return true;
}

// Helper to handle disconnect within recv_packet (called with mutex NOT held)
static void handle_recv_disconnect(void) {
    pthread_mutex_lock(&np.mutex);

    // Close socket under mutex protection
    if (np.tcp_fd >= 0) {
        close(np.tcp_fd);
        np.tcp_fd = -1;
    }

    // For host, go back to waiting and restart broadcast
    if (np.mode == NETPLAY_HOST) {
        np.state = NETPLAY_STATE_WAITING;
        np.needs_state_sync = true;
        np.stall_frames = 0;
        snprintf(np.status_msg, sizeof(np.status_msg), "Client left, waiting on %s:%d", np.local_ip, np.port);
        pthread_mutex_unlock(&np.mutex);
        Netplay_restartBroadcast();  // Can be called without mutex
    } else {
        np.state = NETPLAY_STATE_DISCONNECTED;
        snprintf(np.status_msg, sizeof(np.status_msg), "Remote disconnected");
        pthread_mutex_unlock(&np.mutex);
    }
}

static bool recv_packet(PacketHeader* hdr, void* data, uint16_t max_size, int timeout_ms) {
    if (np.tcp_fd < 0) return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(np.tcp_fd, &fds);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };

    if (select(np.tcp_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
        return false;  // Timeout or error
    }

    ssize_t ret = recv(np.tcp_fd, hdr, sizeof(*hdr), 0);
    if (ret == 0) {
        // Connection closed by remote end
        handle_recv_disconnect();
        return false;
    }
    if (ret < 0 || ret != sizeof(*hdr)) {
        // Error or partial read
        if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
            handle_recv_disconnect();
        }
        return false;
    }

    hdr->frame = ntohl(hdr->frame);
    hdr->size = ntohs(hdr->size);

    // Validate packet size to prevent malformed packet issues
    if (hdr->size > 4096) {
        return false;  // Reject suspiciously large packets
    }

    if (hdr->size > 0 && data && hdr->size <= max_size) {
        ret = recv(np.tcp_fd, data, hdr->size, 0);
        if (ret == 0) {
            handle_recv_disconnect();
            return false;
        }
        if (ret != hdr->size) {
            return false;
        }
    }

    return true;
}

/*
 * NextUI Netplay Helper Module
 * Extracted UI helpers and orchestration functions for netplay menus
 *
 * This module contains the UI rendering and orchestration code that was
 * previously in minarch.c. It's been extracted to keep minarch.c focused
 * on core emulator functionality.
 */

#ifndef NETPLAY_HELPER_H
#define NETPLAY_HELPER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "minarch.h"
#include "netplay.h"
#include "gbalink.h"
#include "gblink.h"

// Link type enum for unified handling of all link types
typedef enum {
    LINK_TYPE_NETPLAY,
    LINK_TYPE_GBALINK,
    LINK_TYPE_GBLINK
} LinkType;

// Result of checking core link support capabilities
typedef struct {
    bool show_netplay;   // true if any link type is supported
    bool has_netpacket;  // true if GBALink (netpacket interface) is supported
    bool has_gblink;     // true if GBLink (gambatte network) is supported
} CoreLinkSupport;

//////////////////////////////////////////////////////////////////////////////
// State Variables (defined in netplay_helper.c, used by minarch.c)
//////////////////////////////////////////////////////////////////////////////

// Force resume flags - set when connection succeeds to auto-close menus
extern int netplay_force_resume;
extern int gbalink_force_resume;
extern int gblink_force_resume;

// Track if client connected to hotspot (for WiFi restoration on disconnect)
extern int netplay_connected_to_hotspot;
extern int gbalink_connected_to_hotspot;
extern int gblink_connected_to_hotspot;

// Store the hotspot SSID client connected to (shared - only one game runs at a time)
extern char connected_hotspot_ssid[33];

// Non-blocking async hotspot stop + WiFi restoration
// Use this when host disconnects via menu to avoid 5-10 second delays
// is_host: true if we were hosting (need to stop hotspot), false if client (just restore WiFi)
void stopHotspotAndRestoreWiFiAsync(bool is_host);

//////////////////////////////////////////////////////////////////////////////
// WiFi/Network Helpers
//////////////////////////////////////////////////////////////////////////////

/**
 * Ensure WiFi is enabled before netplay/link operations
 * Shows UI during WiFi enable process
 * @return true if WiFi is enabled and ready, false if user cancelled or timeout
 */
bool ensureWifiEnabled(void);

/**
 * Check network connectivity for WiFi mode
 * Shows error message if not connected
 * @param type Link type for error message context
 * @param action Action string for error message (e.g., "hosting", "joining")
 * @return true if connected, false if not (shows error message)
 */
bool ensureNetworkConnected(LinkType type, const char* action);

//////////////////////////////////////////////////////////////////////////////
// UI Helpers
//////////////////////////////////////////////////////////////////////////////

/**
 * Show a centered message over darkened game background
 * @param msg Message to display
 */
void showOverlayMessage(const char* msg);

/**
 * Show "Connected!" success screen with timeout
 * @param timeout_ms How long to show the screen (can be dismissed with A)
 */
void showConnectedSuccess(uint32_t timeout_ms);

/**
 * Show mode selection UI for WiFi/Hotspot choice
 * @param title Title to display (e.g., "Host Game", "Join Game")
 * @return 0 = Hotspot, 1 = WiFi, -1 = Cancelled.
 */
int Menu_selectConnectionMode(const char* title);

//////////////////////////////////////////////////////////////////////////////
// Host Discovery State Accessors
// These provide uniform access to the type-specific host arrays
//////////////////////////////////////////////////////////////////////////////

const char* getHostGameName(LinkType type, int index);
const char* getHostIP(LinkType type, int index);
int getHostPort(LinkType type, int index);
int getHostCount(LinkType type);
void setHostCount(LinkType type, int count);
int isLinkConnected(LinkType type);
int* getForceResumeFlag(LinkType type);

/**
 * Check if any multiplayer session is active (Netplay, GBALink, or GBLink)
 * @return 1 if any link type is connected, 0 otherwise
 */
int Multiplayer_isActive(void);

/**
 * Check which link types a core supports
 * @param core_name Core name (e.g., "gpsp", "gambatte", "fbneo")
 * @return CoreLinkSupport struct with support flags
 */
CoreLinkSupport checkCoreLinkSupport(const char* core_name);

//////////////////////////////////////////////////////////////////////////////
// Rendering Helpers
//////////////////////////////////////////////////////////////////////////////

/**
 * Show "Searching for hosts..." screen
 */
void showSearchingScreen(void);

/**
 * Show "Connecting to {ip}..." screen
 * @param host_ip IP address being connected to
 */
void showConnectingScreen(const char* host_ip);

/**
 * Render host selection list with pills (standardized UI)
 * @param type Link type for host list
 * @param selected Currently selected index
 * @param host_count Number of hosts to display
 */
void renderHostSelectionList(LinkType type, int selected, int host_count);

//////////////////////////////////////////////////////////////////////////////
// Orchestration Functions
//////////////////////////////////////////////////////////////////////////////

/**
 * Common host game implementation for all link types
 * Handles mode selection, WiFi enable, and delegates to WiFi/Hotspot specific functions
 * @param type Link type
 * @param list Menu list (unused, for callback signature)
 * @param i Menu item index (unused)
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int hostGame_common(LinkType type, void* list, int i);

/**
 * Common WiFi host implementation for all link types
 * @param type Link type
 * @param game_name Game name for discovery
 * @param crc Game CRC for verification
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int hostGameWiFi_common(LinkType type, const char* game_name, uint32_t crc);

/**
 * Common hotspot host implementation for all link types
 * @param type Link type
 * @param game_name Game name for discovery
 * @param crc Game CRC for verification
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int hostGameHotspot_common(LinkType type, const char* game_name, uint32_t crc);

/**
 * Common join game implementation for all link types
 * Handles mode selection and delegates to WiFi/Hotspot specific functions
 * @param type Link type
 * @param list Menu list (unused, for callback signature)
 * @param i Menu item index (unused)
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int joinGame_common(LinkType type, void* list, int i);

/**
 * Common WiFi join implementation for all link types
 * Handles discovery and host selection
 * @param type Link type
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int joinGameWiFi_common(LinkType type);

/**
 * Unified hotspot join implementation for all link types
 * @param type Link type
 * @return MENU_CALLBACK_NOP or MENU_CALLBACK_EXIT
 */
int joinGame_Hotspot_common(LinkType type);

/**
 * Common disconnect implementation for all link types
 * @param type Link type
 * @param list Menu list (unused, for callback signature)
 * @param i Menu item index (unused)
 * @return MENU_CALLBACK_NOP
 */
int disconnect_common(LinkType type, void* list, int i);

//////////////////////////////////////////////////////////////////////////////
// Utility Functions
//////////////////////////////////////////////////////////////////////////////

/**
 * Calculate simple CRC from game data
 * @return CRC32-like checksum
 */
uint32_t calculateGameCRC(void);

/**
 * Get game name from current ROM (without extension)
 * @param buf Buffer to receive game name
 * @param buf_size Size of buffer
 */
void getGameName(char* buf, size_t buf_size);

/**
 * Show a transition message (e.g., "Disconnecting...", "Connecting...")
 * @param message Message to display
 */
void showTransitionMessage(const char* message);

/**
 * Show a timed confirmation message that can be dismissed with A/B
 * @param message Message to display
 * @param duration_ms Duration to show (can be dismissed early)
 */
void showTimedConfirmation(const char* message, int duration_ms);

/**
 * Format and display link status message
 */
void showLinkStatusMessage(
    const char* title,
    const char* mode_str,
    const char* conn_str,
    const char* state_str,
    const char* code,        // NULL if not hotspot host
    const char* local_ip,
    const char* status_msg
);

/**
 * Render link menu UI
 * @param title Menu title
 * @param items Array of menu item labels
 * @param item_count Number of items
 * @param selected Currently selected index
 * @param getHint Optional hint function, can be NULL
 */
void renderLinkMenuUI(
    const char* title,
    char** items,
    int item_count,
    int selected,
    const char* (*getHint)(void)
);

//////////////////////////////////////////////////////////////////////////////
// Status & Menu Functions
//////////////////////////////////////////////////////////////////////////////

/**
 * Unified status display for all link types
 * @param type Link type
 * @return MENU_CALLBACK_NOP
 */
int status_common(LinkType type);

// Menu option handlers for each link type
int OptionNetplay_hostGame(void* list, int i);
int OptionNetplay_joinGame(void* list, int i);
int OptionNetplay_disconnect(void* list, int i);
int OptionNetplay_status(void* list, int i);

int OptionGBALink_hostGame(void* list, int i);
int OptionGBALink_joinGame(void* list, int i);
int OptionGBALink_disconnect(void* list, int i);
int OptionGBALink_status(void* list, int i);

int OptionGBLink_hostGame(void* list, int i);
int OptionGBLink_joinGame(void* list, int i);
int OptionGBLink_disconnect(void* list, int i);
int OptionGBLink_status(void* list, int i);

/**
 * Get contextual hints for netplay menus (GBA-specific hints)
 * @return Hint string or NULL if no hint applicable
 */
const char* getNetplayMenuHint(void);

/**
 * Get the hint function for a given link type
 * @param type Link type
 * @return Hint function pointer or NULL
 */
const char* (*getLinkMenuHint(LinkType type))(void);

/**
 * Main netplay/link menu handler
 * @param type Link type to show menu for
 * @return 1 if should resume game immediately (force_resume), 0 otherwise
 */
int Netplay_menu_link(LinkType type);

//////////////////////////////////////////////////////////////////////////////
// Hotspot Waiting Screen Helpers
//////////////////////////////////////////////////////////////////////////////

/**
 * Render hotspot host waiting screen with code
 * @param code 4-character hotspot code
 */
void renderHotspotWaitingScreen(const char* code);

/**
 * Render WiFi host waiting screen with IP address
 * @param ip Local IP address
 */
void renderWiFiWaitingScreen(const char* ip);

/**
 * Show connection success screen (3-second countdown, skippable with A)
 */
void showConnectionSuccessScreen(void);

/**
 * Clean up all link sessions (GBLink, GBALink, Netplay)
 * Call before quit to ensure clean shutdown
 */
void Netplay_quitAll(void);

#endif /* NETPLAY_HELPER_H */

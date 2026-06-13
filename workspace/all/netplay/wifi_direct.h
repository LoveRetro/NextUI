/*
 * WiFi Direct - wpa_cli based WiFi operations for netplay
 * Uses wpa_cli directly, bypassing wifi_daemon for more reliable operation
 */

#ifndef WIFI_DIRECT_H
#define WIFI_DIRECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Maximum SSID length
#define WIFI_DIRECT_SSID_MAX 33

// Unified SSID prefix for all link hotspots (shared with minarch via network_common.h)
#ifndef LINK_HOTSPOT_SSID_PREFIX
#define LINK_HOTSPOT_SSID_PREFIX "NextUI-"
#endif

// Hotspot configuration
#define WIFI_DIRECT_HOTSPOT_IP "10.0.0.1"
#define WIFI_DIRECT_HOTSPOT_PASS "nextui123"

// Network info structure for scan results
typedef struct {
    char ssid[WIFI_DIRECT_SSID_MAX];
    int rssi;           // Signal strength (negative dBm, e.g., -50 is strong, -90 is weak)
    bool is_secured;    // true if WPA/WPA2/WEP
    bool has_saved_creds; // true if we have saved credentials for this network
} WIFI_direct_network_t;

//////////////////////////////////
// WiFi Client Functions (wlan0)
//////////////////////////////////

// Ensure WiFi hardware and wpa_supplicant are ready
// Returns true if WiFi is ready for operations
bool WIFI_direct_ensureReady(void);

// Trigger a WiFi scan (non-blocking, just starts the scan)
// Call this, then wait ~1.5 seconds before calling WIFI_direct_scanNetworks
void WIFI_direct_triggerScan(void);

// Scan for all available WiFi networks (reads cached results from last trigger)
// Returns number of networks found
int WIFI_direct_scanNetworks(WIFI_direct_network_t* networks, int max_count);

// Get current connected SSID
// Returns 0 on success with SSID in ssid_out, -1 if not connected
int WIFI_direct_getCurrentSSID(char* ssid_out, size_t ssid_size);

// Check if WiFi is connected using wpa_cli
bool WIFI_direct_isConnected(void);

// Connect to a WiFi network using wpa_cli
// pass can be NULL for open networks
// Returns 0 on success, -1 on failure
int WIFI_direct_connect(const char* ssid, const char* pass);

// Disconnect from current network
void WIFI_direct_disconnect(void);

// Forget (remove) a saved network by SSID
void WIFI_direct_forget(const char* ssid);

// Forget ALL saved netplay hotspot networks (current + legacy prefixes) and
// re-enable other saved networks. Purges stale entries left by aborted sessions.
// Returns the number of networks removed.
int WIFI_direct_forgetAllHotspots(void);

// Scan for hotspots matching a prefix
// Returns number of hotspots found
// ssids_out should be an array of char[WIFI_DIRECT_SSID_MAX]
int WIFI_direct_scanForHotspots(const char* prefix, char ssids_out[][WIFI_DIRECT_SSID_MAX], int max_count);

// Get IP address of wlan0
// Returns 0 on success, -1 on failure
int WIFI_direct_getIP(char* ip_out, size_t ip_size);

// Save current WiFi connection info for later restoration
void WIFI_direct_saveCurrentConnection(void);

// Restore previous WiFi connection after hotspot/link session
// Returns true on success
bool WIFI_direct_restorePreviousConnection(void);

//////////////////////////////////
// Hotspot Functions (wlan0 AP Mode)
//////////////////////////////////

// Start a WiFi hotspot with given SSID and password
// Returns 0 on success, -1 on failure
int WIFI_direct_startHotspot(const char* ssid, const char* password);

// Stop the WiFi hotspot
// Returns 0 on success
int WIFI_direct_stopHotspot(void);

// Check if hotspot is currently active
bool WIFI_direct_isHotspotActive(void);

// Get the hotspot's IP address (always 10.0.0.1)
const char* WIFI_direct_getHotspotIP(void);

// Get the current hotspot SSID
const char* WIFI_direct_getHotspotSSID(void);

// Get the hotspot SSID prefix (LINK_HOTSPOT_SSID_PREFIX)
const char* WIFI_direct_getHotspotSSIDPrefix(void);

// Get the hotspot password
const char* WIFI_direct_getHotspotPassword(void);

#endif // WIFI_DIRECT_H

/*
 * WiFi Direct - netplay WiFi helper
 *
 * Client-side operations (scan / connect / disconnect / IP / save+restore)
 * delegate to the platform WiFi stack (common/generic_wifi.c, exposed via the
 * WIFI_* / PLAT_wifi* API in api.h) so netplay does not ship a second wpa_cli
 * client implementation.
 *
 * Only the hotspot/AP side is implemented here: the platform stack has no
 * Access Point support, and netplay *hosting* needs one (hostapd + udhcpd on
 * wlan0, handing the interface between wpa_supplicant and hostapd by role).
 */

#include "wifi_direct.h"
#include "defines.h"
#include "api.h" // WIFI_*/PLAT_wifi* client API, WIFI_network/WIFI_connection, LOG_*

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Upper bound on networks pulled from a single platform scan (stack buffer).
#define WD_SCAN_MAX 64

// Static state for hotspot
static bool hotspot_active = false;
static char hotspot_ssid[WIFI_DIRECT_SSID_MAX] = {0};
static char hotspot_previous_ssid[128] = {0};

// Helper to sleep in milliseconds
static void wifi_sleep_ms(int ms) {
    usleep(ms * 1000);
}

// Request a DHCP lease on wlan0 and wait (briefly) for a usable IP. Needed when
// joining a host's hotspot, whose udhcpd serves the 10.0.0.x range -- the platform
// connect path associates but does not run a DHCP client itself.
static bool wifi_acquire_dhcp(void) {
    // Drop any stale IP AND routes first. Leaving the old address makes us mistake
    // it for the new lease; leaving the old routes (e.g. "default via 192.168.1.1"
    // from home wifi) means we can't actually reach the host at 10.0.0.1 even once
    // udhcpc assigns 10.0.0.x. udhcpc then installs the correct address + routes.
    system("ip addr flush dev wlan0 2>/dev/null");
    system("ip route flush dev wlan0 2>/dev/null");
    system("killall udhcpc 2>/dev/null");
    wifi_sleep_ms(500); // let the freshly-associated link settle before DHCP

    // Run udhcpc SYNCHRONOUSLY (foreground), not backgrounded: a "udhcpc ... &"
    // launched via system() did not reliably obtain/apply a lease, but running it in
    // the foreground does. -n = exit if no lease, -q = quit once obtained. The first
    // DISCOVER right after association is often dropped, so retry a few times.
    for (int attempt = 0; attempt < 3; attempt++) {
        if (system("udhcpc -i wlan0 -n -q -t 6 2>/dev/null") == 0) {
            char ip[32];
            if (WIFI_direct_getIP(ip, sizeof(ip)) == 0 && ip[0] && strcmp(ip, "0.0.0.0") != 0)
                return true;
        }
        wifi_sleep_ms(300);
    }
    return false;
}

//////////////////////////////////
// WiFi Client Functions (wlan0) - thin wrappers over the platform WiFi stack
//////////////////////////////////

bool WIFI_direct_ensureReady(void) {
    // Bring the platform WiFi stack up if it is currently disabled. wifi_init.sh
    // (run by PLAT_wifiEnable) brings up wlan0 and starts wpa_supplicant.
    if (!WIFI_enabled()) {
        WIFI_enable(true);
        wifi_sleep_ms(1000); // give the supplicant a moment to come up
    }
    return WIFI_enabled();
}

void WIFI_direct_triggerScan(void) {
    // No-op: PLAT_wifiScan() triggers, waits, and reads results in one blocking call.
}

int WIFI_direct_scanNetworks(WIFI_direct_network_t* networks, int max_count) {
    if (!networks || max_count <= 0) return 0;

    struct WIFI_network found[WD_SCAN_MAX];
    int want = max_count < WD_SCAN_MAX ? max_count : WD_SCAN_MAX;
    int n = WIFI_scan(found, want);
    if (n < 0) return 0;

    int count = 0;
    for (int i = 0; i < n && count < max_count; i++) {
        if (found[i].ssid[0] == '\0') continue;
        strncpy(networks[count].ssid, found[i].ssid, WIFI_DIRECT_SSID_MAX - 1);
        networks[count].ssid[WIFI_DIRECT_SSID_MAX - 1] = '\0';
        networks[count].rssi = found[i].rssi;
        networks[count].is_secured = (found[i].security != SECURITY_NONE);
        networks[count].has_saved_creds = WIFI_isKnown(found[i].ssid, found[i].security);
        count++;
    }
    return count;
}

int WIFI_direct_getCurrentSSID(char* ssid_out, size_t ssid_size) {
    if (!ssid_out || ssid_size < 1) return -1;
    ssid_out[0] = '\0';

    struct WIFI_connection info;
    if (WIFI_connectionInfo(&info) != 0 || !info.valid || info.ssid[0] == '\0') return -1;
    strncpy(ssid_out, info.ssid, ssid_size - 1);
    ssid_out[ssid_size - 1] = '\0';
    return 0;
}

bool WIFI_direct_isConnected(void) {
    return WIFI_connected();
}

void WIFI_direct_saveCurrentConnection(void) {
    struct WIFI_connection info;
    if (WIFI_connectionInfo(&info) == 0 && info.valid && info.ssid[0] != '\0') {
        strncpy(hotspot_previous_ssid, info.ssid, sizeof(hotspot_previous_ssid) - 1);
        hotspot_previous_ssid[sizeof(hotspot_previous_ssid) - 1] = '\0';
    }
}

int WIFI_direct_connect(const char* ssid, const char* pass) {
    if (!ssid) return -1;

    bool has_pass = (pass && pass[0] != '\0');
    WIFI_connectPass(ssid, has_pass ? SECURITY_WPA2_PSK : SECURITY_NONE, has_pass ? pass : NULL);

    // WIFI_connectPass uses enable_network, which leaves other saved networks (e.g.
    // home wifi) enabled. When joining a host hotspot the home network is usually
    // higher priority, so wpa_supplicant associates to it instead -- we then sit on
    // the wrong subnet and can't reach the host at 10.0.0.1. Force an exclusive
    // selection of the requested SSID, then confirm we actually landed on it:
    // WIFI_connected() alone only checks wpa_state, not which network we joined.
    WIFI_selectOnly(ssid);

    bool on_target = false;
    char current[128];
    for (int i = 0; i < 20; i++) { // up to ~10s for the (re)association to settle
        if (WIFI_connected() &&
            WIFI_direct_getCurrentSSID(current, sizeof(current)) == 0 &&
            strcmp(current, ssid) == 0) {
            on_target = true;
            break;
        }
        wifi_sleep_ms(500);
    }

    if (!on_target) {
        LOG_error("WIFI_direct_connect: failed to associate to '%s'\n", ssid);
        return -1;
    }

    // Associated to the right AP; pull a DHCP lease from the host's udhcpd (best effort).
    if (!wifi_acquire_dhcp()) {
        LOG_error("WIFI_direct_connect: DHCP timeout - no IP assigned\n");
    }
    return 0;
}

void WIFI_direct_disconnect(void) {
    WIFI_disconnect();
}

void WIFI_direct_forget(const char* ssid) {
    if (!ssid || !ssid[0]) return;
    WIFI_forget((char*)ssid, SECURITY_WPA2_PSK);
}

// Remove ALL saved netplay hotspot networks, not just the current session's. Each
// join adds a uniquely-named hotspot network; sessions that don't end cleanly leave
// theirs behind, so without a prefix sweep wpa_supplicant.conf accumulates dozens of
// stale NextUI-*/GBLink-*/GBALink-* entries. Also re-enables any networks a prior
// select_network() left disabled so the saved config stays clean. Returns count removed.
int WIFI_direct_forgetAllHotspots(void) {
    WIFI_enableAll();
    int removed = 0;
    removed += WIFI_forgetPrefix(LINK_HOTSPOT_SSID_PREFIX); // current: "NextUI-"
    removed += WIFI_forgetPrefix("GBLink-");                // legacy
    removed += WIFI_forgetPrefix("GBALink-");               // legacy
    return removed;
}

int WIFI_direct_scanForHotspots(const char* prefix, char ssids_out[][WIFI_DIRECT_SSID_MAX], int max_count) {
    if (!prefix || !ssids_out || max_count <= 0) return 0;

    size_t prefix_len = strlen(prefix);
    struct WIFI_network found[WD_SCAN_MAX];
    int count = 0;

    // A couple of passes since hotspots can take a moment to appear.
    for (int retry = 0; retry < 3 && count == 0; retry++) {
        int n = WIFI_scan(found, WD_SCAN_MAX);
        if (n < 0) continue;
        for (int i = 0; i < n && count < max_count; i++) {
            if (strncmp(found[i].ssid, prefix, prefix_len) == 0) {
                strncpy(ssids_out[count], found[i].ssid, WIFI_DIRECT_SSID_MAX - 1);
                ssids_out[count][WIFI_DIRECT_SSID_MAX - 1] = '\0';
                count++;
            }
        }
    }
    return count;
}

int WIFI_direct_getIP(char* ip_out, size_t ip_size) {
    if (!ip_out || ip_size < 16) return -1;
    ip_out[0] = '\0';

    struct WIFI_connection info;
    if (WIFI_connectionInfo(&info) != 0 || !info.valid || info.ip[0] == '\0') return -1;
    strncpy(ip_out, info.ip, ip_size - 1);
    ip_out[ip_size - 1] = '\0';
    return 0;
}

bool WIFI_direct_restorePreviousConnection(void) {
    if (hotspot_previous_ssid[0] == '\0') return false;

    // Reconnect using the credentials wpa_supplicant already has for this SSID.
    WIFI_connect(hotspot_previous_ssid, SECURITY_WPA2_PSK);

    for (int i = 0; i < 20; i++) { // up to ~10s
        wifi_sleep_ms(500);
        if (WIFI_connected()) {
            char current[128];
            if (WIFI_direct_getCurrentSSID(current, sizeof(current)) == 0 &&
                strcmp(current, hotspot_previous_ssid) == 0) {
                wifi_acquire_dhcp();
                hotspot_previous_ssid[0] = '\0';
                return true;
            }
        }
    }

    hotspot_previous_ssid[0] = '\0';
    return false;
}

//////////////////////////////////
// Hotspot Functions (wlan0 AP Mode) - no platform equivalent, kept here
//////////////////////////////////

// The AP runs on wlan0 directly. A device is only ever host OR client in a
// session, so the host hands wlan0 from the client stack (wpa_supplicant) to
// hostapd, then hands it back on stop. This works on single-radio devices
// (tg5050, only wlan0) and on tg5040 (wlan0 + a boot-created wlan1 AP vif that
// must be removed first, since the radio allows only one AP interface).
int WIFI_direct_startHotspot(const char* ssid, const char* password) {
    if (hotspot_active) {
        return 0;
    }

    // Save the current client connection (while wpa_supplicant is still up) so we
    // can restore it after hosting.
    WIFI_direct_saveCurrentConnection();

    // Clean up any leftover hotspot processes from a previous session.
    system("killall hostapd 2>/dev/null");
    system("killall udhcpd 2>/dev/null");

    // Free the single AP slot: drop any pre-existing wlan1 AP vif (tg5040). No-op
    // where it doesn't exist (tg5050).
    system("iw dev wlan1 del 2>/dev/null");

    // Hand wlan0 to hostapd: stop the client stack so hostapd can take exclusive
    // nl80211 control of the interface.
    system("killall wpa_supplicant 2>/dev/null");
    system("killall udhcpc 2>/dev/null");
    wifi_sleep_ms(300);

    // Reset wlan0 and bring it up for AP mode.
    system("ip addr flush dev wlan0 2>/dev/null");
    system("ip link set wlan0 down 2>/dev/null");
    wifi_sleep_ms(100);
    system("ip link set wlan0 up 2>/dev/null");
    wifi_sleep_ms(200);

    // Create hostapd config
    FILE* f = fopen("/tmp/gbalink_hostapd.conf", "w");
    if (!f) {
        LOG_error("WIFI_direct_startHotspot: failed to create hostapd config\n");
        WIFI_enable(true); // restore the client stack
        return -1;
    }
    fprintf(f, "interface=wlan0\n");
    fprintf(f, "driver=nl80211\n");
    fprintf(f, "ssid=%s\n", ssid);
    fprintf(f, "channel=6\n");
    fprintf(f, "hw_mode=g\n");
    fprintf(f, "auth_algs=1\n");
    fprintf(f, "wpa=2\n");
    fprintf(f, "wpa_passphrase=%s\n", password);
    fprintf(f, "wpa_key_mgmt=WPA-PSK\n");
    fprintf(f, "rsn_pairwise=CCMP\n");
    fclose(f);

    // Create udhcpd config
    f = fopen("/tmp/gbalink_udhcpd.conf", "w");
    if (!f) {
        LOG_error("WIFI_direct_startHotspot: failed to create udhcpd config\n");
        WIFI_enable(true);
        return -1;
    }
    fprintf(f, "start 10.0.0.10\n");
    fprintf(f, "end 10.0.0.50\n");
    fprintf(f, "interface wlan0\n");
    fprintf(f, "pidfile /tmp/gbalink_udhcpd.pid\n");
    fprintf(f, "lease_file /tmp/gbalink_udhcpd.leases\n");
    fprintf(f, "option subnet 255.255.255.0\n");
    fprintf(f, "option router 10.0.0.1\n");
    fclose(f);

    // Start hostapd (switches wlan0 into AP mode).
    int ret = system("hostapd -B /tmp/gbalink_hostapd.conf");
    if (ret != 0) {
        LOG_error("WIFI_direct_startHotspot: failed to start hostapd\n");
        system("ip link set wlan0 down 2>/dev/null");
        WIFI_enable(true);
        return -1;
    }

    // Assign the AP IP on wlan0.
    system("ip addr add 10.0.0.1/24 dev wlan0 2>/dev/null");

    // Start DHCP server
    ret = system("udhcpd /tmp/gbalink_udhcpd.conf");
    if (ret != 0) {
        LOG_error("WIFI_direct_startHotspot: failed to start udhcpd\n");
        system("killall hostapd 2>/dev/null");
        system("ip addr flush dev wlan0 2>/dev/null");
        system("ip link set wlan0 down 2>/dev/null");
        WIFI_enable(true);
        return -1;
    }

    strncpy(hotspot_ssid, ssid, sizeof(hotspot_ssid) - 1);
    hotspot_ssid[sizeof(hotspot_ssid) - 1] = '\0';
    hotspot_active = true;
    return 0;
}

int WIFI_direct_stopHotspot(void) {
    if (!hotspot_active) {
        return 0;
    }

    // Stop the AP and DHCP server.
    system("killall hostapd 2>/dev/null");
    wifi_sleep_ms(200);  // wait for hostapd to release wlan0
    system("kill $(cat /tmp/gbalink_udhcpd.pid 2>/dev/null) 2>/dev/null");
    system("killall udhcpd 2>/dev/null");

    // Tear down the AP addressing on wlan0.
    system("ip addr flush dev wlan0 2>/dev/null");
    system("ip link set wlan0 down 2>/dev/null");

    // Cleanup temp files
    system("rm -f /tmp/gbalink_*.conf /tmp/gbalink_*.pid /tmp/gbalink_*.leases 2>/dev/null");

    hotspot_active = false;
    hotspot_ssid[0] = '\0';
    // NOTE: Don't clear hotspot_previous_ssid here - it's needed by
    // WIFI_direct_restorePreviousConnection() which is called later

    // Hand wlan0 back to the client stack: WIFI_enable(true) runs the platform's
    // wifi_init.sh start, which restarts wpa_supplicant (+ dhcp) and reconnects to
    // known networks. WIFI_direct_restorePreviousConnection() (called by the netplay
    // flow afterwards) re-selects the saved network.
    WIFI_enable(true);
    wifi_sleep_ms(500);

    return 0;
}

bool WIFI_direct_isHotspotActive(void) {
    return hotspot_active;
}

const char* WIFI_direct_getHotspotIP(void) {
    return WIFI_DIRECT_HOTSPOT_IP;
}

const char* WIFI_direct_getHotspotSSID(void) {
    return hotspot_ssid;
}

const char* WIFI_direct_getHotspotSSIDPrefix(void) {
    return LINK_HOTSPOT_SSID_PREFIX;
}

const char* WIFI_direct_getHotspotPassword(void) {
    return WIFI_DIRECT_HOTSPOT_PASS;
}

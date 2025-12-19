/////////////////////////////////////////////////////////////////////////////////////////

// File: common/generic_wifi.c
// Generic implementations of wifi functions, to be used by platforms that don't
// provide their own implementations.
// Used by: tg5050
// Library dependencies: none
// Tool dependencies: wpa_cli, wpa_supplicant, iproute2 (ip command)
// Script dependencies: $SYSTEM_PATH/etc/wifi/wifi_init.sh

// \note This files does not have an acompanying header, as all functions are declared in api.h
// with minimal fallback implementations
// \sa FALLBACK_IMPLEMENTATION

/////////////////////////////////////////////////////////////////////////////////////////

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

bool PLAT_hasWifi() { return true; }

#define WIFI_INTERFACE "wlan0"
#define WPA_CLI_CMD "wpa_cli -p /etc/wifi/sockets -i " WIFI_INTERFACE

#define wifilog(fmt, ...) \
    LOG_note(PLAT_wifiDiagnosticsEnabled() ? LOG_INFO : LOG_DEBUG, fmt, ##__VA_ARGS__)

// Helper function to run a command and capture output
static int wifi_run_cmd(const char *cmd, char *output, size_t output_len) {
    wifilog("Running command: %s\n", cmd);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        LOG_error("wifi_run_cmd: failed to run command: %s\n", cmd);
        return -1;
    }
    
    if (output && output_len > 0) {
        output[0] = '\0';
        size_t total = 0;
        char buf[256];
        while (fgets(buf, sizeof(buf), fp) && total < output_len - 1) {
            size_t len = strlen(buf);
            if (total + len >= output_len) {
                len = output_len - total - 1;
            }
            memcpy(output + total, buf, len);
            total += len;
        }
        output[total] = '\0';
    }
    
    int status = pclose(fp);
    int exit_code = WEXITSTATUS(status);
    wifilog("Command exit code: %d\n", exit_code);
    return exit_code;
}

// Helper to check if wpa_supplicant is running
static bool wifi_supplicant_running(void) {
    return system("pidof wpa_supplicant > /dev/null 2>&1") == 0;
}

// Helper to get IP address of wifi interface
static bool wifi_get_ip(char *ip, size_t len) {
    char cmd[256];
    char output[256];
    snprintf(cmd, sizeof(cmd), "ip -4 addr show %s 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2", WIFI_INTERFACE);
    if (wifi_run_cmd(cmd, output, sizeof(output)) == 0 && output[0] != '\0') {
        trimTrailingNewlines(output);
        strncpy(ip, output, len - 1);
        ip[len - 1] = '\0';
        return true;
    }
    ip[0] = '\0';
    return false;
}

void PLAT_wifiInit() {
	wifilog("Wifi init\n");
	PLAT_wifiEnable(CFG_getWifi());
}

bool PLAT_wifiEnabled() {
	return CFG_getWifi();
}

void PLAT_wifiEnable(bool on) {
	if (on) {
		wifilog("turning wifi on...\n");
		system(SYSTEM_PATH "/etc/wifi/wifi_init.sh start");
		// Keep config in sync
		CFG_setWifi(on);
	}
	else {
		wifilog("turning wifi off...\n");
		// Keep config in sync
		CFG_setWifi(on);
		system(SYSTEM_PATH "/etc/wifi/wifi_init.sh stop");
	}
}

int PLAT_wifiScan(struct WIFI_network *networks, int max)
{
    if (!CFG_getWifi()) {
        LOG_error("PLAT_wifiScan: wifi is currently disabled.\n");
        return -1;
    }

    wifilog("PLAT_wifiScan: Starting WiFi scan...\n");
    // Trigger a scan
    system(WPA_CLI_CMD " scan 2>/dev/null");
    wifilog("PLAT_wifiScan: Waiting 2s for scan to complete...\n");
	usleep(2000000); // Give time for scan to complete

    wifilog("PLAT_wifiScan: Retrieving scan results...\n");
    // Get scan results
    char results[16384];
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s scan_results 2>/dev/null", WPA_CLI_CMD);
    if (wifi_run_cmd(cmd, results, sizeof(results)) != 0) {
        LOG_error("PLAT_wifiScan: failed to get scan results.\n");
        return -1;
    }

    // wpa_cli scan_results format:
    // bssid / frequency / signal level / flags / ssid
    // 04:b4:fe:32:f9:73	2462	-63	[WPA2-PSK-CCMP][WPS][ESS]	frynet

    wifilog("%s\n", results);

    const char *current = results;

    // Skip header line
    const char *next = strchr(current, '\n');
    if (!next) {
        LOG_warn("PLAT_wifiScan: no scan results lines found.\n");
        return 0;
    }
    current = next + 1;

    int count = 0;
    char line[512];

    while (current && *current && count < max) {
        next = strchr(current, '\n');
        size_t len = next ? (size_t)(next - current) : strlen(current);
        if (len >= sizeof(line)) {
            LOG_warn("PLAT_wifiScan: line too long, truncating.\n");
            len = sizeof(line) - 1;
        }

        strncpy(line, current, len);
        line[len] = '\0';

        char features[128];
        struct WIFI_network *network = &networks[count];

        // Initialize fields
        network->bssid[0] = '\0';
        network->ssid[0] = '\0';
        network->freq = -1;
        network->rssi = -1;
        network->security = SECURITY_NONE;

        int parsed = sscanf(line, "%17[0-9a-fA-F:]\t%d\t%d\t%127[^\t]\t%127[^\n]",
                            network->bssid, &network->freq, &network->rssi,
                            features, network->ssid);

        if (parsed < 4) {
            LOG_warn("PLAT_wifiScan: malformed line skipped (parsed %d fields): '%s'\n", parsed, line);
            current = next ? next + 1 : NULL;
            continue;
        }

        // Trim trailing whitespace from SSID
        size_t ssid_len = strlen(network->ssid);
        while (ssid_len > 0 && (network->ssid[ssid_len - 1] == ' ' || network->ssid[ssid_len - 1] == '\t')) {
            network->ssid[ssid_len - 1] = '\0';
            ssid_len--;
        }

        if (network->ssid[0] == '\0') {
            LOG_warn("Ignoring network %s with empty SSID\n", network->bssid);
            current = next ? next + 1 : NULL;
            continue;
        }

        if (containsString(features, "WPA2-PSK"))
            network->security = SECURITY_WPA2_PSK;
        else if (containsString(features, "WPA-PSK"))
            network->security = SECURITY_WPA_PSK;
        else if (containsString(features, "WEP"))
            network->security = SECURITY_WEP;
        else if (containsString(features, "EAP"))
            network->security = SECURITY_UNSUPPORTED;

        count++;
        current = next ? next + 1 : NULL;
    }

    wifilog("PLAT_wifiScan: Found %d networks\n", count);
    return count;
}

bool PLAT_wifiConnected()
{
	if (!CFG_getWifi()) {
		wifilog("PLAT_wifiConnected: wifi is currently disabled.\n");
		return false;
	}

	wifilog("PLAT_wifiConnected: Checking WiFi connection status...\n");
	char output[256];
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "%s status 2>/dev/null | grep '^wpa_state=' | cut -d= -f2", WPA_CLI_CMD);
	if (wifi_run_cmd(cmd, output, sizeof(output)) != 0) {
		return false;
	}
	
	trimTrailingNewlines(output);
	wifilog("PLAT_wifiConnected: wifi state is %s\n", output);
	
	return strcmp(output, "COMPLETED") == 0;
}

int PLAT_wifiConnection(struct WIFI_connection *connection_info)
{
	if (!CFG_getWifi()) {
		wifilog("PLAT_wifiConnection: wifi is currently disabled.\n");
		connection_reset(connection_info);
		return -1;
	}

	wifilog("PLAT_wifiConnection: Retrieving connection details...\n");
	// Get status from wpa_cli
	char status[2048];
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "%s status 2>/dev/null", WPA_CLI_CMD);
	if (wifi_run_cmd(cmd, status, sizeof(status)) != 0) {
		connection_reset(connection_info);
		return -1;
	}

	// Parse wpa_state
	char *state_line = strstr(status, "wpa_state=");
	if (!state_line || strstr(state_line, "COMPLETED") == NULL) {
		connection_reset(connection_info);
		wifilog("PLAT_wifiConnection: Not connected\n");
		return 0;
	}

	// We're connected, fill in the info
	connection_info->valid = true;
	
	// Parse SSID
	wifilog("PLAT_wifiConnection: Parsing connection info...\n");
	char *ssid_line = strstr(status, "\nssid=");
	if (ssid_line) {
		ssid_line += 6; // skip "\nssid="
		char *end = strchr(ssid_line, '\n');
		size_t len = end ? (size_t)(end - ssid_line) : strlen(ssid_line);
		if (len >= SSID_MAX) len = SSID_MAX - 1;
		strncpy(connection_info->ssid, ssid_line, len);
		connection_info->ssid[len] = '\0';
	} else {
		connection_info->ssid[0] = '\0';
	}

	// Parse frequency
	char *freq_line = strstr(status, "\nfreq=");
	if (freq_line) {
		connection_info->freq = atoi(freq_line + 6);
	} else {
		connection_info->freq = -1;
	}

	// Get IP address
	wifi_get_ip(connection_info->ip, sizeof(connection_info->ip));

	// Get signal strength from iw
	wifilog("PLAT_wifiConnection: Retrieving signal strength...\n");
	connection_info->rssi = -1;
	connection_info->link_speed = -1;
	connection_info->noise = -1;
	
	snprintf(cmd, sizeof(cmd), "iw dev %s link 2>/dev/null", WIFI_INTERFACE);
	char link_info[1024];
	if (wifi_run_cmd(cmd, link_info, sizeof(link_info)) == 0) {
		// Parse signal: -XX dBm
		char *signal = strstr(link_info, "signal:");
		if (signal) {
			connection_info->rssi = atoi(signal + 7);
		}
		// Parse tx bitrate: XXX.X MBit/s
		char *bitrate = strstr(link_info, "tx bitrate:");
		if (bitrate) {
			connection_info->link_speed = (int)atof(bitrate + 11);
		}
	}

	wifilog("Connected AP: %s\n", connection_info->ssid);
	wifilog("IP address: %s\n", connection_info->ip);
	wifilog("Signal strength: %d dBm, Link speed: %d Mbps\n", connection_info->rssi, connection_info->link_speed);

	return 0;
}

bool PLAT_wifiHasCredentials(char *ssid, WifiSecurityType sec)
{
    // Validate input SSID (reject tabs/newlines)
    for (int i = 0; ssid[i]; ++i) {
        if (ssid[i] == '\t' || ssid[i] == '\n') {
            LOG_warn("PLAT_wifiHasCredentials: SSID contains invalid control characters.\n");
            return false;
        }
    }

    if (!CFG_getWifi()) {
        LOG_error("PLAT_wifiHasCredentials: wifi is currently disabled.\n");
        return false;
    }

    // Get list of configured networks from wpa_cli
    char list_results[4096];
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s list_networks 2>/dev/null", WPA_CLI_CMD);
    if (wifi_run_cmd(cmd, list_results, sizeof(list_results)) != 0) {
        wifilog("PLAT_wifiHasCredentials: failed to get network list.\n");
        return false;
    }

    wifilog("LIST:\n%s\n", list_results);

    // wpa_cli list_networks format:
    // network id / ssid / bssid / flags
    // 0	MyNetwork	any	[CURRENT]

    const char *current = list_results;

    // Skip header line
    const char *next = strchr(current, '\n');
    if (!next) {
        LOG_warn("PLAT_wifiHasCredentials: network list has no data lines.\n");
        return false;
    }
    current = next + 1;

    char line[256];

    while (current && *current) {
        next = strchr(current, '\n');
        size_t len = next ? (size_t)(next - current) : strlen(current);
        if (len >= sizeof(line)) {
            LOG_warn("PLAT_wifiHasCredentials: line too long, truncating.\n");
            len = sizeof(line) - 1;
        }

        strncpy(line, current, len);
        line[len] = '\0';

        wifilog("Parsing line: '%s'\n", line);

        // Tokenize line by tabs
        char *saveptr = NULL;
        char *token_id    = strtok_r(line, "\t", &saveptr);
        char *token_ssid  = strtok_r(NULL, "\t", &saveptr);

        if (!(token_id && token_ssid)) {
            LOG_warn("PLAT_wifiHasCredentials: Malformed line skipped: '%s'\n", line);
            current = next ? next + 1 : NULL;
            continue;
        }

        if (strcmp(token_ssid, ssid) == 0) {
            return true;
        }

        current = next ? next + 1 : NULL;
    }

    return false;
}

// Helper to find network ID by SSID
static int wifi_find_network_id(const char *ssid) {
    wifilog("wifi_find_network_id: Looking for network '%s'...\n", ssid);
    char list_results[4096];
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "%s list_networks 2>/dev/null", WPA_CLI_CMD);
    if (wifi_run_cmd(cmd, list_results, sizeof(list_results)) != 0) {
        wifilog("wifi_find_network_id: Failed to get network list\n");
        return -1;
    }

    const char *current = list_results;
    const char *next = strchr(current, '\n');
    if (!next) return -1;
    current = next + 1;

    char line[256];
    while (current && *current) {
        next = strchr(current, '\n');
        size_t len = next ? (size_t)(next - current) : strlen(current);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        strncpy(line, current, len);
        line[len] = '\0';

        char *saveptr = NULL;
        char *token_id   = strtok_r(line, "\t", &saveptr);
        char *token_ssid = strtok_r(NULL, "\t", &saveptr);

        if (token_id && token_ssid && strcmp(token_ssid, ssid) == 0) {
            int id = atoi(token_id);
            wifilog("wifi_find_network_id: Found network '%s' with id %d\n", ssid, id);
            return id;
        }

        current = next ? next + 1 : NULL;
    }

    wifilog("wifi_find_network_id: Network '%s' not found\n", ssid);
    return -1;
}

void PLAT_wifiForget(char *ssid, WifiSecurityType sec)
{
	if (!CFG_getWifi()) {
		LOG_error("PLAT_wifiForget: wifi is currently disabled.\n");
		return;
	}

	int network_id = wifi_find_network_id(ssid);
	if (network_id >= 0) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd), "%s remove_network %d 2>/dev/null", WPA_CLI_CMD, network_id);
		system(cmd);
		system(WPA_CLI_CMD " save_config 2>/dev/null");
		wifilog("PLAT_wifiForget: removed network %s (id=%d)\n", ssid, network_id);
	} else {
		wifilog("PLAT_wifiForget: network %s not found\n", ssid);
	}
}

void PLAT_wifiConnect(char *ssid, WifiSecurityType sec)
{
	PLAT_wifiConnectPass(ssid, sec, NULL);
}

void PLAT_wifiConnectPass(const char *ssid, WifiSecurityType sec, const char* pass)
{
	if (!CFG_getWifi()) {
		wifilog("PLAT_wifiConnectPass: wifi is currently disabled.\n");
		return;
	}

	if (ssid == NULL) {
		// Disconnect request
		wifilog("PLAT_wifiConnectPass: Disconnecting from WiFi...\n");
		system(WPA_CLI_CMD " disconnect 2>/dev/null");
		wifilog("PLAT_wifiConnectPass: disconnected\n");
		return;
	}

	wifilog("PLAT_wifiConnectPass: Attempting to connect to SSID '%s' (security=%d)\n", ssid, sec);

	// Check if network already exists
	int network_id = wifi_find_network_id(ssid);
	char cmd[512];
	char output[128];
	
	if (network_id < 0) {
		// Add new network
		snprintf(cmd, sizeof(cmd), "%s add_network 2>/dev/null", WPA_CLI_CMD);
		if (wifi_run_cmd(cmd, output, sizeof(output)) != 0) {
			LOG_error("PLAT_wifiConnectPass: failed to add network\n");
			return;
		}
		network_id = atoi(output);
		wifilog("Added new network with id %d\n", network_id);
		
		// Set SSID (needs quotes for wpa_cli)
		wifilog("Setting network SSID...\n");
		snprintf(cmd, sizeof(cmd), "%s set_network %d ssid '\"%s\"' 2>/dev/null", WPA_CLI_CMD, network_id, ssid);
		system(cmd);
		
		// Set password or open network
		if (pass && pass[0] != '\0') {
			wifilog("Setting network password...\n");
			snprintf(cmd, sizeof(cmd), "%s set_network %d psk '\"%s\"' 2>/dev/null", WPA_CLI_CMD, network_id, pass);
			system(cmd);
		} else if (sec == SECURITY_NONE) {
			wifilog("Configuring as open network...\n");
			snprintf(cmd, sizeof(cmd), "%s set_network %d key_mgmt NONE 2>/dev/null", WPA_CLI_CMD, network_id);
			system(cmd);
		}
		
		// Save configuration
		wifilog("Saving network configuration...\n");
		system(WPA_CLI_CMD " save_config 2>/dev/null");
	} else if (pass && pass[0] != '\0') {
		// Update password for existing network
		wifilog("Updating password for existing network...\n");
		snprintf(cmd, sizeof(cmd), "%s set_network %d psk '\"%s\"' 2>/dev/null", WPA_CLI_CMD, network_id, pass);
		system(cmd);
		system(WPA_CLI_CMD " save_config 2>/dev/null");
	} else {
		wifilog("Using existing network configuration...\n");
	}
	
	// Enable and select network
	wifilog("Enabling and selecting network %d...\n", network_id);
	snprintf(cmd, sizeof(cmd), "%s enable_network %d 2>/dev/null", WPA_CLI_CMD, network_id);
	system(cmd);
	snprintf(cmd, sizeof(cmd), "%s select_network %d 2>/dev/null", WPA_CLI_CMD, network_id);
	system(cmd);
	
	// Wait for connection
	wifilog("Waiting for connection (up to 5 seconds)...\n");
	for (int i = 0; i < 10; i++) {
		usleep(500000);
		if (PLAT_wifiConnected()) {
			wifilog("PLAT_wifiConnectPass: connected successfully after %d attempts\n", i + 1);
			// Request IP via DHCP
			wifilog("Requesting IP address via DHCP...\n");
			char dhcp_cmd[128];
			snprintf(dhcp_cmd, sizeof(dhcp_cmd), "udhcpc -i %s -n -q 2>/dev/null &", WIFI_INTERFACE);
			system(dhcp_cmd);
			return;
		}
	}
	
	LOG_error("PLAT_wifiConnectPass: connection timeout after 5 seconds\n");
}

void PLAT_wifiDisconnect()
{
	PLAT_wifiConnectPass(NULL, SECURITY_WPA2_PSK, NULL);
}

bool PLAT_wifiDiagnosticsEnabled() 
{
	return CFG_getWifiDiagnostics();
}

void PLAT_wifiDiagnosticsEnable(bool on) 
{
	CFG_setWifiDiagnostics(on);
}
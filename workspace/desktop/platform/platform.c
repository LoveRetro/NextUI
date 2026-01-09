// desktop
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#include <msettings.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"

#include <dirent.h>

static SDL_Joystick *joystick;
void PLAT_initInput(void) {
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	joystick = SDL_JoystickOpen(0);
}
void PLAT_quitInput(void) {
	SDL_JoystickClose(joystick);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

///////////////////////////////

static int online = 1;
void PLAT_updateNetworkStatus()
{
	online = 1;
}

void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	PLAT_getBatteryStatusFine(is_charging, charge);
}

void PLAT_getBatteryStatusFine(int* is_charging, int* charge)
{
	*is_charging = 1;
	*charge = 100;
}

void PLAT_enableBacklight(int enable) {
	// buh
}

void PLAT_powerOff(int reboot) {
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();
	exit(0);
}

///////////////////////////////

void PLAT_setCPUSpeed(int speed) {
	// buh
}

void PLAT_setRumble(int strength) {
	// buh
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	return "Desktop";
}

void PLAT_getOsVersionInfo(char *output_str, size_t max_len)
{
	sprintf(output_str, "%s", "1.2.3");
}

int PLAT_isOnline(void) {
	return online;
}

ConnectionStrength PLAT_connectionStrength(void) {
	return SIGNAL_STRENGTH_HIGH;
}

/////////////////////////////////
// Remove, just for debug


#define MAX_LINE_LENGTH 200
#define ZONE_PATH "/var/db/timezone/zoneinfo"
#define ZONE_TAB_PATH ZONE_PATH "/zone.tab"

static char cached_timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
static int cached_tz_count = -1;

int compare_timezones(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void PLAT_initTimezones() {
    if (cached_tz_count != -1) { // Already initialized
        return;
    }
    
    FILE *file = fopen(ZONE_TAB_PATH, "r");
    if (!file) {
        LOG_info("Error opening file %s\n", ZONE_TAB_PATH);
        return;
    }
    
    char line[MAX_LINE_LENGTH];
    cached_tz_count = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // Skip comment lines
        if (line[0] == '#' || strlen(line) < 3) {
            continue;
        }
        
        char *token = strtok(line, "\t"); // Skip country code
        if (!token) continue;
        
        token = strtok(NULL, "\t"); // Skip latitude/longitude
        if (!token) continue;
        
        token = strtok(NULL, "\t\n"); // Extract timezone
        if (!token) continue;
        
        // Check for duplicates before adding
        int duplicate = 0;
        for (int i = 0; i < cached_tz_count; i++) {
            if (strcmp(cached_timezones[i], token) == 0) {
                duplicate = 1;
                break;
            }
        }
        
        if (!duplicate && cached_tz_count < MAX_TIMEZONES) {
            strncpy(cached_timezones[cached_tz_count], token, MAX_TZ_LENGTH - 1);
            cached_timezones[cached_tz_count][MAX_TZ_LENGTH - 1] = '\0'; // Ensure null-termination
            cached_tz_count++;
        }
    }
    
    fclose(file);
    
    // Sort the list alphabetically
    qsort(cached_timezones, cached_tz_count, MAX_TZ_LENGTH, compare_timezones);
}

void PLAT_getTimezones(char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH], int *tz_count) {
    if (cached_tz_count == -1) {
        LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
        *tz_count = 0;
        return;
    }
    
    memcpy(timezones, cached_timezones, sizeof(cached_timezones));
    *tz_count = cached_tz_count;
}

char *PLAT_getCurrentTimezone() {
	// call readlink -f /tmp/localtime to get the current timezone path, and
	// then remove /usr/share/zoneinfo/ from the beginning of the path to get the timezone name.
	char *tz_path = (char *)malloc(256);
	if (!tz_path) {
		return NULL;
	}
	if (readlink("/etc/localtime", tz_path, 256) == -1) {
		free(tz_path);
		return NULL;
	}
	tz_path[255] = '\0'; // Ensure null-termination
	char *tz_name = strstr(tz_path, ZONE_PATH "/");
	if (tz_name) {
		tz_name += strlen(ZONE_PATH "/");
		return strdup(tz_name);
	} else {
		return strdup(tz_path);
	}
}

void PLAT_setCurrentTimezone(const char* tz) {
	return;
	if (cached_tz_count == -1)
	{
		LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
        return;
	}

	// tzset()

	// tz will be in format Asia/Shanghai
	char *tz_path = (char *)malloc(256);
	if (!tz_path) {
		return;
	}
	snprintf(tz_path, 256, ZONE_PATH "/%s", tz);
	if (unlink("/tmp/localtime") == -1) {
		LOG_error("Failed to remove existing symlink: %s\n", strerror(errno));
	}
	if (symlink(tz_path, "/tmp/localtime") == -1) {
		LOG_error("Failed to set timezone: %s\n", strerror(errno));
	}
	free(tz_path);
}

/////////////////////

 void PLAT_wifiInit() {}
 bool PLAT_hasWifi() { return true; }
 bool PLAT_wifiEnabled() { return true; }
 void PLAT_wifiEnable(bool on) {}

 int PLAT_wifiScan(struct WIFI_network *networks, int max) {
	for (int i = 0; i < 5; i++) {
		struct WIFI_network *network = &networks[i];

		sprintf(network->ssid, "Network%d", i);
		strcpy(network->bssid, "01:01:01:01:01:01");
		network->rssi = (70 / 5) * (i + 1);
		network->freq = 2400;
		network->security = i % 2 ? SECURITY_WPA2_PSK : SECURITY_WEP;
	}
	return 5;
 }
 bool PLAT_wifiConnected() { return true; }
 int PLAT_wifiConnection(struct WIFI_connection *connection_info) {
	connection_info->freq = 2400;
	strcpy(connection_info->ip, "127.0.0.1");
	strcpy(connection_info->ssid, "Network1");
	return 0;
 }
 bool PLAT_wifiHasCredentials(char *ssid, WifiSecurityType sec) { return false; }
 void PLAT_wifiForget(char *ssid, WifiSecurityType sec) {}
 void PLAT_wifiConnect(char *ssid, WifiSecurityType sec) {}
 void PLAT_wifiConnectPass(const char *ssid, WifiSecurityType sec, const char* pass) {}
 void PLAT_wifiDisconnect() {}

 /////////////////////////

// We use the generic video implementation here
#include "generic_video.c"
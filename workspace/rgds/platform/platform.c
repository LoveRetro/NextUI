#define _GNU_SOURCE
// rgds: Anbernic RG DS (RK3568) on the ROCKNIX kernel
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>

#include <msettings.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"
#include "scaler.h"

static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;
void PLAT_initInput(void) {
	if(SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0)
		LOG_error("Failed initializing joysticks: %s\n", SDL_GetError());
	num_joysticks = SDL_NumJoysticks();
    if (num_joysticks > 0) {
        joysticks = (SDL_Joystick **)malloc(sizeof(SDL_Joystick *) * num_joysticks);
        for (int i = 0; i < num_joysticks; i++) {
			joysticks[i] = SDL_JoystickOpen(i);
			LOG_info("Opening joystick %d: %s\n", i, SDL_JoystickName(joysticks[i]));
        }
    }
}

void PLAT_quitInput(void) {
	if (joysticks) {
        for (int i = 0; i < num_joysticks; i++) {
            if (SDL_JoystickGetAttached(joysticks[i])) {
				LOG_info("Closing joystick %d: %s\n", i, SDL_JoystickName(joysticks[i]));
				SDL_JoystickClose(joysticks[i]);
			}
        }
        free(joysticks);
        joysticks = NULL;
        num_joysticks = 0;
    }
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

void PLAT_updateInput(const SDL_Event *event) {
	switch (event->type) {
    case SDL_JOYDEVICEADDED: {
        int device_index = event->jdevice.which;
        SDL_Joystick *new_joy = SDL_JoystickOpen(device_index);
        if (new_joy) {
            joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * (num_joysticks + 1));
            joysticks[num_joysticks++] = new_joy;
            LOG_info("Joystick added at index %d: %s\n", device_index, SDL_JoystickName(new_joy));
        } else {
            LOG_error("Failed to open added joystick at index %d: %s\n", device_index, SDL_GetError());
        }
        break;
    }

    case SDL_JOYDEVICEREMOVED: {
        SDL_JoystickID removed_id = event->jdevice.which;
        for (int i = 0; i < num_joysticks; ++i) {
            if (SDL_JoystickInstanceID(joysticks[i]) == removed_id) {
                LOG_info("Joystick removed: %s\n", SDL_JoystickName(joysticks[i]));
                SDL_JoystickClose(joysticks[i]);

                // Shift down the remaining entries
                for (int j = i; j < num_joysticks - 1; ++j)
                    joysticks[j] = joysticks[j + 1];
                num_joysticks--;

                if (num_joysticks == 0) {
                    free(joysticks);
                    joysticks = NULL;
                } else {
                    joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * num_joysticks);
                }
                break;
            }
        }
        break;
    }

    default:
        break;
    }
}

///////////////////////////////

// Battery and charger are found by type rather than name: rk817 / cw2015 names vary between kernels.
static int powerSupplyPath(const char* type, const char* attr, char* out, size_t len) {
	DIR* dir = opendir("/sys/class/power_supply");
	if (!dir) return 0;
	struct dirent* e;
	int found = 0;
	while (!found && (e = readdir(dir))) {
		if (e->d_name[0] == '.') continue;
		char path[256], value[32] = {0};
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
		getFile(path, value, sizeof(value));
		if (strncmp(value, type, strlen(type)) == 0) {
			snprintf(out, len, "/sys/class/power_supply/%s/%s", e->d_name, attr);
			found = exists(out);
		}
	}
	closedir(dir);
	return found;
}

void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	PLAT_getBatteryStatusFine(is_charging, charge);

	// worry less about battery and more about the game you're playing
	     if (*charge>80) *charge = 100;
	else if (*charge>60) *charge =  80;
	else if (*charge>40) *charge =  60;
	else if (*charge>20) *charge =  40;
	else if (*charge>10) *charge =  20;
	else           		 *charge =  10;
}

void PLAT_getBatteryStatusFine(int *is_charging, int *charge)
{
	char path[256];
	if (is_charging) {
		char status[32] = {0};
		if (powerSupplyPath("Battery", "status", path, sizeof(path))) getFile(path, status, sizeof(status));
		*is_charging = strncmp(status, "Charging", 8) == 0 || strncmp(status, "Full", 4) == 0;
	}
	if (charge) {
		*charge = powerSupplyPath("Battery", "capacity", path, sizeof(path)) ? getInt(path) : 100;
	}
}

void PLAT_getCPUTemp() {
	perf.cpu_temp = getInt("/sys/class/thermal/thermal_zone0/temp")/1000;
}

void PLAT_getCPUSpeed() {
	perf.cpu_speed = getInt("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq")/1000;
}

void PLAT_getGPUTemp() {
	perf.gpu_temp = getInt("/sys/class/thermal/thermal_zone1/temp")/1000;
}

void PLAT_getGPUSpeed() {
	perf.gpu_speed = getInt("/sys/devices/platform/fde60000.gpu/devfreq/fde60000.gpu/cur_freq")/1000000;
}

int PLAT_isUSBConnected(void) { return 0; }

void PLAT_enableBacklight(int enable) {
	if (enable) {
		SetBrightness(GetBrightness());
	}
	else {
		SetRawBrightness(0);
	}
}

void PLAT_powerOff(int reboot) {
	if (CFG_getHaptics()) {
		VIB_singlePulse(VIB_bootStrength, VIB_bootDuration_ms);
	}
	system("rm -f /tmp/nextui_exec && sync");
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();

	system("cat /dev/zero > /dev/fb0 2>/dev/null");
	if(reboot > 0)
		touch("/tmp/reboot");
	else
		touch("/tmp/poweroff");
	sync();
	exit(0);
}

// Sleep is owned by powerd (power key / lid), which suspends the whole system.
int PLAT_supportsDeepSleep(void) { return 0; }

///////////////////////////////

void PLAT_setCPUSpeed(int speed) {
	const char* governor;
	switch (speed) {
		case CPU_SPEED_PERFORMANCE: governor = "performance"; break;
		case CPU_SPEED_POWERSAVE: governor = "powersave"; break;
		case CPU_SPEED_AUTO: governor = "schedutil"; break;
		default: return;
	}
	putFile("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", (char*)governor);
}

void PLAT_setRumble(int strength) {
	// pwm-vibrator is a force-feedback evdev device; ponytail: no rumble yet, add FF_RUMBLE when a game needs it
	(void)strength;
}

int PLAT_pickSampleRate(int requested, int max) {
	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	return "Anbernic RG DS";
}

void PLAT_getOsVersionInfo(char* output_str, size_t max_len) {
	getFile("/etc/nextui-duo-version", output_str, max_len);
}

// Wifi/bluetooth: Realtek RTL8821CS (SDIO wifi via rtw88_8821cs, UART bluetooth via hci_uart/btrtl),
// driven by the generic wpa_supplicant/bluez implementations below. Status as on tg5050.
static struct WIFI_connection connection = {
	.valid = false, .freq = -1, .link_speed = -1, .noise = -1, .rssi = -1, .ip = {0}, .ssid = {0},
};
static bool bluetoothConnected = false;

static inline void connection_reset(struct WIFI_connection *connection_info) {
	connection_info->valid = false;
	connection_info->freq = -1;
	connection_info->link_speed = -1;
	connection_info->noise = -1;
	connection_info->rssi = -1;
	*connection_info->ip = 0;
	*connection_info->ssid = 0;
}

void PLAT_getNetworkStatus(int* is_online) {
	if (WIFI_enabled())
		WIFI_connectionInfo(&connection);
	else
		connection_reset(&connection);
	if (is_online) *is_online = connection.valid && connection.ssid[0] != '\0';
	bluetoothConnected = BT_enabled() && PLAT_bluetoothConnected();
}

ConnectionStrength PLAT_connectionStrength(void) {
	if (!WIFI_enabled() || !connection.valid || connection.rssi == -1) return SIGNAL_STRENGTH_OFF;
	if (connection.rssi == 0) return SIGNAL_STRENGTH_DISCONNECTED;
	if (connection.rssi >= -60) return SIGNAL_STRENGTH_HIGH;
	if (connection.rssi >= -70) return SIGNAL_STRENGTH_MED;
	return SIGNAL_STRENGTH_LOW;
}

bool PLAT_btIsConnected(void) {
	return bluetoothConnected;
}

/////////////////////////

// Bottom panel companion; routes generic_video's window calls (see bottom.c)
#include "bottom.c"

// We use the generic video implementation here
#include "generic_video.c"

/////////////////////////

// Generic wifi (wpa_supplicant) and bluetooth (bluez) implementations; scripts in SYSTEM_PATH/etc.
#define WIFI_SOCK_DIR "/tmp/wifi/sockets"
#include "generic_wifi.c"

#include "generic_bt.c"

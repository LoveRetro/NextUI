// tg5050
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
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
#include <time.h>
#include <pthread.h>

#include <dirent.h>

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

void PLAT_getCPUTemp() {
	currentcputemp = getInt("/sys/devices/virtual/thermal/thermal_zone0/temp")/1000;

}

static struct WIFI_connection connection = {
	.valid = false,
	.freq = -1,
	.link_speed = -1,
	.noise = -1,
	.rssi = -1,
	.ip = {0},
	.ssid = {0},
};

static inline void connection_reset(struct WIFI_connection *connection_info)
{
	connection_info->valid = false;
	connection_info->freq = -1;
	connection_info->link_speed = -1;
	connection_info->noise = -1;
	connection_info->rssi = -1;
	*connection_info->ip = '\0';
	*connection_info->ssid = '\0';
}

static bool bluetoothConnected = false;

void PLAT_updateNetworkStatus()
{
	if(WIFI_enabled())
		WIFI_connectionInfo(&connection);
	else
		connection_reset(&connection);
	
	if(BT_enabled()) {
		bluetoothConnected = PLAT_bluetoothConnected();
	}
	else
		bluetoothConnected = false;
}

void PLAT_getBatteryStatusFine(int *is_charging, int *charge)
{	
	if(is_charging) {
		int time_to_full = getInt("/sys/class/power_supply/axp2202-battery/time_to_full_now");
		int charger_present = getInt("/sys/class/power_supply/axp2202-usb/online"); 
		*is_charging = (charger_present == 1) && (time_to_full > 0);
	}
	if(charge) {
		*charge = getInt("/sys/class/power_supply/axp2202-battery/capacity");
	}
}

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

int PLAT_supportsDeepSleep(void) { return 1; }

///////////////////////////////

double get_time_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9; // Convert to seconds
}
double get_process_cpu_time_sec() {
	// this gives cpu time in nanoseconds needed to accurately calculate cpu usage in very short time frames. 
	// unfortunately about 20ms between meassures seems the lowest i can go to get accurate results
	// maybe in the future i will find and even more granual way to get cpu time, but might just be a limit of C or Linux alltogether
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9; // Convert to seconds
}

static pthread_mutex_t currentcpuinfo;
// a roling average for the display values of about 2 frames, otherwise they are unreadable jumping too fast up and down and stuff to read
#define ROLLING_WINDOW 120  

//volatile int useAutoCpu = 1;
void *PLAT_cpu_monitor(void *arg) {
    struct timespec start_time, curr_time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start_time);

    long clock_ticks_per_sec = sysconf(_SC_CLK_TCK);

    double prev_real_time = get_time_sec();
    double prev_cpu_time = get_process_cpu_time_sec();

	// little Cortex-A55 CPU0 - 408Mhz to 1416Mhz
	// 408000 672000 792000 936000 1032000 1128000 1224000 1320000 1416000
	// big Cortex-A55 CPU4 - 408Mhz to 2160Mhz
	// 408000 672000 840000 1008000 1200000 1344000 1488000 1584000 1680000 1800000 1992000 2088000 2160000
	const int cpu_frequencies[] = {408,672,840,1008,1200,1344,1488,1584,1680,1800,1992,2088,2160};
    const int num_freqs = sizeof(cpu_frequencies) / sizeof(cpu_frequencies[0]);
    int current_index = 1; // 672Mhz start 

    double cpu_usage_history[ROLLING_WINDOW] = {0};
    double cpu_speed_history[ROLLING_WINDOW] = {0};
    int history_index = 0;
    int history_count = 0; 

    while (true) {
        if (useAutoCpu) {
            double curr_real_time = get_time_sec();
            double curr_cpu_time = get_process_cpu_time_sec();

            double elapsed_real_time = curr_real_time - prev_real_time;
            double elapsed_cpu_time = curr_cpu_time - prev_cpu_time;
            double cpu_usage = 0;

            if (elapsed_real_time > 0) {
                cpu_usage = (elapsed_cpu_time / elapsed_real_time) * 100.0;
            }

            pthread_mutex_lock(&currentcpuinfo);

			// the goal here is is to keep cpu usage between 75% and 85% at the lowest possible speed so device stays cool and battery usage is at a minimum
			// if usage falls out of this range it will either scale a step down or up 
			// but if usage hits above 95% we need that max boost and we instant scale up to 2000mhz as long as needed
			// all this happens very fast like 60 times per second, so i'm applying roling averages to display values, so debug screen is readable and gives a good estimate on whats happening cpu wise
			// the roling averages are purely for displaying, the actual scaling is happening realtime each run. 
            if (cpu_usage > 95) {
                current_index = num_freqs - 1; // Instant power needed, cpu is above 95% Jump directly to max boost 2000MHz
            }
            else if (cpu_usage > 85 && current_index < num_freqs - 1) { // otherwise try to keep between 75 and 85 at lowest clock speed
                current_index++; 
            } 
            else if (cpu_usage < 75 && current_index > 0) {
                current_index--; 
            }

            PLAT_setCustomCPUSpeed(cpu_frequencies[current_index] * 1000);

            cpu_usage_history[history_index] = cpu_usage;
            cpu_speed_history[history_index] = cpu_frequencies[current_index];

            history_index = (history_index + 1) % ROLLING_WINDOW;
            if (history_count < ROLLING_WINDOW) {
                history_count++; 
            }

            double sum_cpu_usage = 0, sum_cpu_speed = 0;
            for (int i = 0; i < history_count; i++) {
                sum_cpu_usage += cpu_usage_history[i];
                sum_cpu_speed += cpu_speed_history[i];
            }

            currentcpuse = sum_cpu_usage / history_count;
            currentcpuspeed = sum_cpu_speed / history_count;

            pthread_mutex_unlock(&currentcpuinfo);

            prev_real_time = curr_real_time;
            prev_cpu_time = curr_cpu_time;
			// 20ms really seems lowest i can go, anything lower it becomes innacurate, maybe one day I will find another even more granual way to calculate usage accurately and lower this shit to 1ms haha, altough anything lower than 10ms causes cpu usage in itself so yeah
			// Anyways screw it 20ms is pretty much on a frame by frame basis anyways, so will anything lower really make a difference specially if that introduces cpu usage by itself? 
			// Who knows, maybe some CPU engineer will find my comment here one day and can explain, maybe this is looking for the limits of C and needs Assambler or whatever to call CPU instructions directly to go further, but all I know is PUSH and MOV, how did the orignal Roller Coaster Tycoon developer wrote a whole game like this anyways? Its insane..
            usleep(20000);
        } else {
            // Just measure CPU usage without changing frequency
            double curr_real_time = get_time_sec();
            double curr_cpu_time = get_process_cpu_time_sec();

            double elapsed_real_time = curr_real_time - prev_real_time;
            double elapsed_cpu_time = curr_cpu_time - prev_cpu_time;

            if (elapsed_real_time > 0) {
                double cpu_usage = (elapsed_cpu_time / elapsed_real_time) * 100.0;

                pthread_mutex_lock(&currentcpuinfo);

                cpu_usage_history[history_index] = cpu_usage;

                history_index = (history_index + 1) % ROLLING_WINDOW;
                if (history_count < ROLLING_WINDOW) {
                    history_count++;
                }

                double sum_cpu_usage = 0;
                for (int i = 0; i < history_count; i++) {
                    sum_cpu_usage += cpu_usage_history[i];
                }

                currentcpuse = sum_cpu_usage / history_count;

                pthread_mutex_unlock(&currentcpuinfo);
            }

            prev_real_time = curr_real_time;
            prev_cpu_time = curr_cpu_time;
            usleep(100000); 
        }
    }
}


#define GOVERNOR_PATH "/sys/devices/system/cpu/cpu4/cpufreq/scaling_setspeed"
void PLAT_setCustomCPUSpeed(int speed) {
    FILE *fp = fopen(GOVERNOR_PATH, "w");
    if (fp == NULL) {
        perror("Failed to open scaling_setspeed");
        return;
    }

    fprintf(fp, "%d\n", speed);
    fclose(fp);
}
void PLAT_setCPUSpeed(int speed) {
	int freq = 0;
	switch (speed) {
		case CPU_SPEED_MENU: 		freq =  672000; currentcpuspeed = 672; break;
		case CPU_SPEED_POWERSAVE:	freq = 1200000; currentcpuspeed = 1200; break;
		case CPU_SPEED_NORMAL: 		freq = 1680000; currentcpuspeed = 1680; break;
		case CPU_SPEED_PERFORMANCE: freq = 2160000; currentcpuspeed = 2160; break;
	}
	putInt(GOVERNOR_PATH, freq);
}

#define MAX_STRENGTH 0xFFFF
#define RUMBLE_LEVEL_PATH "/sys/class/motor/level"

void PLAT_setRumble(int strength) {
	if(strength > 0 && strength < MAX_STRENGTH) {
		putInt(RUMBLE_LEVEL_PATH, strength);
	}
	else {
		putInt(RUMBLE_LEVEL_PATH, MAX_STRENGTH);
	}
}

int PLAT_pickSampleRate(int requested, int max) {
	// bluetooth: allow limiting the maximum to improve compatibility
	if(PLAT_bluetoothConnected())
		return MIN(requested, CFG_getBluetoothSamplingrateLimit());

	return MIN(requested, max);
}

char* PLAT_getModel(void) {
	char* model = getenv("TRIMUI_MODEL");
	if (model) return model;
	return "Trimui Smart Pro S";
}

void PLAT_getOsVersionInfo(char* output_str, size_t max_len)
{
	return getFile("/etc/version", output_str,max_len);
}

bool PLAT_btIsConnected(void)
{
	return bluetoothConnected;
}

int PLAT_isOnline(void) {
	return (connection.valid && connection.ssid[0] != '\0');
}

ConnectionStrength PLAT_connectionStrength(void) {
	if(!WIFI_enabled() || !connection.valid || connection.rssi == -1)
		return SIGNAL_STRENGTH_OFF;
	else if (connection.rssi == 0)
		return SIGNAL_STRENGTH_DISCONNECTED;
	else if (connection.rssi >= -60)
		return SIGNAL_STRENGTH_HIGH;
	else if (connection.rssi >= -70)
		return SIGNAL_STRENGTH_MED;
	else
		return SIGNAL_STRENGTH_LOW;
}

void PLAT_initDefaultLeds() {
	lightsDefault[0] = (LightSettings) {
		"Joystick L",
		"l",
		4,
		1000,
		100,
		0xFFFFFF,
		0xFFFFFF,
		0,
		{},
		1,
		100,
		0
	};
	lightsDefault[1] = (LightSettings) {
		"Joystick R",
		"r",
		4,
		1000,
		100,
		0xFFFFFF,
		0xFFFFFF,
		0,
		{},
		1,
		100,
		0
	};
	lightsDefault[2] = (LightSettings) {
		"Logo",
		"m",
		4,
		1000,
		100,
		0xFFFFFF,
		0xFFFFFF,
		0,
		{},
		1,
		100,
		0
	};
}
void PLAT_initLeds(LightSettings *lights) 
{
	PLAT_initDefaultLeds();
	FILE *file = PLAT_OpenSettings("ledsettings.txt");

    if (file == NULL)
    {
        LOG_warn("Unable to open led settings file\n");
    }
	else {
		char line[256];
		int current_light = -1;
		while (fgets(line, sizeof(line), file))
		{
			if (line[0] == '[')
			{
				// Section header
				char light_name[255];
				if (sscanf(line, "[%49[^]]]", light_name) == 1)
				{
					current_light++;
					if (current_light < MAX_LIGHTS)
					{
						strncpy(lights[current_light].name, light_name, 255 - 1);
						lights[current_light].name[255 - 1] = '\0'; // Ensure null-termination
						lights[current_light].cycles = -1; // cycles (times animation loops) should basically always be -1 for unlimited unless specifically set
					}
					else
					{
						LOG_info("Maximum number of lights (%d) exceeded. Ignoring further sections.\n", MAX_LIGHTS);
						current_light = -1; // Reset if max_lights exceeded
					}
				}
			}
			else if (current_light >= 0 && current_light < MAX_LIGHTS)
			{
				int temp_value;
				uint32_t temp_color;
				char filename[255];

				if (sscanf(line, "filename=%s", &filename) == 1)
				{
					strncpy(lights[current_light].filename, filename, 255 - 1);
					continue;
				}
				if (sscanf(line, "effect=%d", &temp_value) == 1)
				{
					lights[current_light].effect = temp_value;
					continue;
				}
				if (sscanf(line, "color1=%x", &temp_color) == 1)
				{
					lights[current_light].color1 = temp_color;
					continue;
				}
				if (sscanf(line, "color2=%x", &temp_color) == 1)
				{
					lights[current_light].color2 = temp_color;
					continue;
				}
				if (sscanf(line, "speed=%d", &temp_value) == 1)
				{
					lights[current_light].speed = temp_value;
					continue;
				}
				if (sscanf(line, "brightness=%d", &temp_value) == 1)
				{
					lights[current_light].brightness = temp_value;
					continue;
				}
				if (sscanf(line, "trigger=%d", &temp_value) == 1)
				{
					lights[current_light].trigger = temp_value;
					continue;
				}
				if (sscanf(line, "inbrightness=%d", &temp_value) == 1)
				{
					lights[current_light].inbrightness = temp_value;
					continue;
				}
			}
		}
		fclose(file);
	}
}

#define LED_PATH1 "/sys/class/led_anim/max_scale"
#define LED_PATH2 "/sys/class/led_anim/max_scale_lr"
#define LED_PATH3 "/sys/class/led_anim/max_scale_f1f2" 

void PLAT_setLedInbrightness(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
	snprintf(filepath, sizeof(filepath), LED_PATH1);
	if (strcmp(led->filename, "f2") != 0) {
		// do nothhing for f2
		file = fopen(filepath, "w");
		if (file != NULL)
		{
			fprintf(file, "%i\n", led->inbrightness);
			fclose(file);
		}
	}
}
void PLAT_setLedBrightness(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
	snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale");
	if (strcmp(led->filename, "f2") != 0) {
		// do nothhing for f2
		file = fopen(filepath, "w");
		if (file != NULL)
		{
			fprintf(file, "%i\n", led->brightness);
			fclose(file);
		}
	}
}
void PLAT_setLedEffect(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
    snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/effect_%s", led->filename);
    file = fopen(filepath, "w");
    if (file != NULL)
    {
        fprintf(file, "%i\n", led->effect);
        fclose(file);
    }
}
void PLAT_setLedEffectCycles(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
    snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/effect_cycles_%s", led->filename);
    file = fopen(filepath, "w");
    if (file != NULL)
    {
        fprintf(file, "%i\n", led->cycles);
        fclose(file);
    }
}
void PLAT_setLedEffectSpeed(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
    snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/effect_duration_%s", led->filename);
    file = fopen(filepath, "w");
    if (file != NULL)
    {
        fprintf(file, "%i\n", led->speed);
        fclose(file);
    }
}
void PLAT_setLedColor(LightSettings *led)
{
    char filepath[256];
    FILE *file;
    // first set brightness
    snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/effect_rgb_hex_%s", led->filename);
    file = fopen(filepath, "w");
    if (file != NULL)
    {
        fprintf(file, "%06X\n", led->color1);
        fclose(file);
    }
}

//////////////////////////////////////////////

bool PLAT_canTurbo(void) { return true; }

#define INPUTD_PATH "/tmp/trimui_inputd"

typedef struct TurboBtnPath {
	int brn_id;
	char *path;
} TurboBtnPath;

static TurboBtnPath turbo_mapping[] = {
    {BTN_ID_A, INPUTD_PATH "/turbo_a"},
    {BTN_ID_B, INPUTD_PATH "/turbo_b"},
    {BTN_ID_X, INPUTD_PATH "/turbo_x"},
    {BTN_ID_Y, INPUTD_PATH "/turbo_y"},
    {BTN_ID_L1, INPUTD_PATH "/turbo_l"},
    {BTN_ID_L2, INPUTD_PATH "/turbo_l2"},
    {BTN_ID_R1, INPUTD_PATH "/turbo_r"},
    {BTN_ID_R2, INPUTD_PATH "/turbo_r2"},
	{0, NULL}
};

int toggle_file(const char *path) {
    if (access(path, F_OK) == 0) {
        unlink(path);
        return 0;
    } else {
        int fd = open(path, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
        return -1; // error
    }
}

int PLAT_toggleTurbo(int btn_id)
{
	// avoid extra file IO on each call
	static int initialized = 0;
	if (!initialized) {
		mkdir(INPUTD_PATH, 0755);
		initialized = 1;
	}

	for (int i = 0; turbo_mapping[i].path; i++) {
		if (turbo_mapping[i].brn_id == btn_id) {
			return toggle_file(turbo_mapping[i].path);
		}
	}
	return 0;
}

void PLAT_clearTurbo() {
	for (int i = 0; turbo_mapping[i].path; i++) {
		unlink(turbo_mapping[i].path);
	}
}

//////////////////////////////////////////////

int PLAT_setDateTime(int y, int m, int d, int h, int i, int s) {
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock -u -w", y,m,d,h,i,s);
	system(cmd);
	return 0; // why does this return an int?
}

#define MAX_LINE_LENGTH 200
#define ZONE_PATH "/usr/share/zoneinfo"
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

	char *output = (char *)malloc(256);
	if (!output) {
		return false;
	}
	FILE *fp = popen("uci get system.@system[0].zonename", "r");
	if (!fp) {
		free(output);
		return false;
	}
	fgets(output, 256, fp);
	pclose(fp);
	trimTrailingNewlines(output);

	return output;
}

void PLAT_setCurrentTimezone(const char* tz) {
	if (cached_tz_count == -1) {
		LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
        return;
    }

	// This makes it permanent
	char *zonename = (char *)malloc(256);
	if (!zonename)
		return;
	snprintf(zonename, 256, "uci set system.@system[0].zonename=\"%s\"", tz);
	system(zonename);
	//system("uci set system.@system[0].zonename=\"Europe/Berlin\"");
	system("uci del -q system.@system[0].timezone");
	system("uci commit system");
	free(zonename);

	// This fixes the timezone until the next reboot
	char *tz_path = (char *)malloc(256);
	if (!tz_path) {
		return;
	}
	snprintf(tz_path, 256, ZONE_PATH "/%s", tz);
	// replace existing symlink
	if (unlink("/tmp/localtime") == -1) {
		LOG_error("Failed to remove existing symlink: %s\n", strerror(errno));
	}
	if (symlink(tz_path, "/tmp/localtime") == -1) {
		LOG_error("Failed to set timezone: %s\n", strerror(errno));
	}
	free(tz_path);

	// apply timezone to kernel
	system("date -k");
}

bool PLAT_getNetworkTimeSync(void) {
	char *output = (char *)malloc(256);
	if (!output) {
		return false;
	}
	FILE *fp = popen("uci get system.ntp.enable", "r");
	if (!fp) {
		free(output);
		return false;
	}
	fgets(output, 256, fp);
	pclose(fp);
	bool result = (output[0] == '1');
	free(output);
	return result;
}

void PLAT_setNetworkTimeSync(bool on) {
	// note: this is not the service residing at /etc/init.d/ntpd - that one has hardcoded time server URLs and does not interact with UCI.
	if (on) {
		// permanment
		system("uci set system.ntp.enable=1");
		system("uci commit system");
		system("/etc/init.d/ntpd reload");
	} else {
		// permanment
		system("uci set system.ntp.enable=0");
		system("uci commit system");
		system("/etc/init.d/ntpd stop");
	}
}

/////////////////////////

// We use the generic video implementation here
#include "generic_video.c"

/////////////////////////

// We use the generic wifi implementation here
#include "generic_wifi.c"

/////////////////////////

bool PLAT_hasBluetooth() { return true; }
bool PLAT_bluetoothEnabled() { return CFG_getBluetooth(); }

/*
#include "bt_dev_list.h"
#include "bt_log.h"
#include "bt_manager.h"

// callbacks should be dameonized
// TODO: can we get away with just implementing those we need?
dev_list_t *bonded_devices = NULL;
dev_list_t *discovered_controllers = NULL;
dev_list_t *discovered_audiodev = NULL;

static bool auto_connect = true;

#define btlog(fmt, ...) \
    LOG_note(PLAT_bluetoothDiagnosticsEnabled() ? LOG_INFO : LOG_DEBUG, fmt, ##__VA_ARGS__)

// Utility: Parse BT device class

#define COD_MAJOR_MASK     0x1F00
#define COD_MINOR_MASK     0x00FC
#define COD_SERVICE_MASK   0xFFE000

#define GET_MAJOR_CLASS(cod) ((cod & COD_MAJOR_MASK) >> 8)
#define GET_MINOR_CLASS(cod) ((cod & COD_MINOR_MASK) >> 2)
#define GET_SERVICE_CLASS(cod) ((cod & COD_SERVICE_MASK) >> 13)

static void bt_test_manager_cb(int event_id)
{
	btlog("bt test callback function enter, event_id: %d", event_id);
}

static void bt_test_adapter_power_state_cb(btmg_adapter_power_state_t state)
{
	if (state == BTMG_ADAPTER_TURN_ON_SUCCESSED) {
		btlog("Turn on bt successfully\n");
	} else if (state == BTMG_ADAPTER_TURN_ON_FAILED) {
		btlog("Failed to turn on bt\n");
	} else if (state == BTMG_ADAPTER_TURN_OFF_SUCCESSED) {
		btlog("Turn off bt successfully\n");
	} else if (state == BTMG_ADAPTER_TURN_OFF_FAILED) {
		btlog("Failed to turn off bt\n");
	}
}

static int is_background = 1;
static void bt_test_status_cb(btmg_state_t status)
{
	if (status == BTMG_STATE_OFF) {
		btlog("BT is off\n");
	} else if (status == BTMG_STATE_ON) {
		btlog("BT is ON\n");
		if(is_background)
			bt_manager_gap_set_io_capability(BTMG_IO_CAP_NOINPUTNOOUTPUT);
		else
			bt_manager_gap_set_io_capability(BTMG_IO_CAP_KEYBOARDDISPLAY);
		bt_manager_set_discovery_mode(BTMG_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
	} else if (status == BTMG_STATE_TURNING_ON) {
		btlog("bt is turnning on.\n");
	} else if (status == BTMG_STATE_TURNING_OFF) {
		btlog("bt is turnning off.\n");
	}
}

static void bt_test_discovery_status_cb(btmg_discovery_state_t status)
{
	if (status == BTMG_DISC_STARTED) {
		btlog("bt start scanning.\n");
	} else if (status == BTMG_DISC_STOPPED_AUTO) {
		btlog("scanning stop automatically\n");
	} else if (status == BTMG_DISC_START_FAILED) {
		btlog("start scan failed.\n");
	} else if (status == BTMG_DISC_STOPPED_BY_USER) {
		btlog("stop scan by user.\n");
	}
}

static void bt_test_gap_connected_changed_cb(btmg_bt_device_t *device)
{
	LOG_info("address:%s,name:%s,class:%d,icon:%s,address type:%s,rssi:%d,state:%s\n",device->remote_address,
			device->remote_name, device->r_class, device->icon, device->address_type, device->rssi, device->connected ? "CONNECTED":"DISCONNECTED");
	// TODO: check for device class and call SDL_OpenJoystick if its a gamepad/HID device
	if(device->connected == false) {
		bt_manager_set_discovery_mode(BTMG_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
	}
	else {
		// not sure why this isnt handled over btmgr interface, but we need it
		char act[256];
		sprintf(act, "bluetoothctl trust %s", device->remote_address);
		system(act);
	}
}

static void bt_test_dev_add_cb(btmg_bt_device_t *device)
{
	btlog("address:%s,name:%s,class:%d,icon:%s,address type:%s,rssi:%d\n", device->remote_address,
			device->remote_name, device->r_class, device->icon, device->address_type, device->rssi);

	//print_bt_class(device->r_class);

	{
		if (GET_MAJOR_CLASS(device->r_class) == 0x04) { // Audio/Video
			if(!btmg_dev_list_find_device(discovered_audiodev, device->remote_address))
				btmg_dev_list_add_device(discovered_audiodev, device->remote_name, device->remote_address);
		}
		if (GET_MAJOR_CLASS(device->r_class) == 0x05) { // Peripheral
			if(!btmg_dev_list_find_device(discovered_controllers, device->remote_address))
				btmg_dev_list_add_device(discovered_controllers, device->remote_name, device->remote_address);
		}
	}
}

static void bt_test_dev_remove_cb(btmg_bt_device_t *device)
{
	btlog("address:%s,name:%s,class:%d,address type:%s\n", device->remote_address,
			device->remote_name, device->r_class,device->address_type);

	{
		btmg_dev_list_remove_device(discovered_audiodev, device->remote_address);
		btmg_dev_list_remove_device(discovered_controllers, device->remote_address);
	}
}

static void bt_test_update_rssi_cb(const char *address, int rssi)
{
	//dev_node_t *dev_node = NULL;

	//pthread_mutex_lock(&discovered_devices_mtx);
	//dev_node = btmg_dev_list_find_device(discovered_devices, address);
	//if (dev_node) {
		// too spammy
		//LOG_info("address:%s,name:%s,rssi:%d\n", dev_node->dev_addr, dev_node->dev_name, rssi);
	//}
	//pthread_mutex_unlock(&discovered_devices_mtx);
}

static void bt_test_bond_state_cb(btmg_bond_state_t state,const  char *bd_addr,const char *name)
{
	btlog("bonded device state:%d, addr:%s, name:%s\n", state, bd_addr, name);
	
	{
		dev_node_t *dev_bonded_node = NULL;
		dev_bonded_node = btmg_dev_list_find_device(bonded_devices, bd_addr);

		if (state == BTMG_BOND_STATE_BONDED) {
			if (dev_bonded_node == NULL) {
				btmg_dev_list_add_device(bonded_devices, name, bd_addr);
			}

			if(btmg_dev_list_find_device(discovered_audiodev, bd_addr)) {
				btmg_dev_list_remove_device(discovered_audiodev, bd_addr);
			}

			if(btmg_dev_list_find_device(discovered_controllers, bd_addr)) {
				btmg_dev_list_remove_device(discovered_controllers, bd_addr);
			}

			btlog("Pairing state for %s is BONDED\n", name);
		} else if (state == BTMG_BOND_STATE_NONE) {
			if (dev_bonded_node != NULL) {
				btmg_dev_list_remove_device(bonded_devices, bd_addr);
			}
			btlog("Pairing state for %s is BOND NONE\n", name);
		} else if (state == BTMG_BOND_STATE_BONDING) {
			btlog("Pairing state for %s is BONDING\n", name);
		}
	}
}
#define BUFFER_SIZE 17
static void bt_test_pair_ask(const char *prompt,char *buffer)
{
	btlog("%s", prompt);
	if (fgets(buffer, BUFFER_SIZE, stdin)  == NULL)
		btlog("cmd fgets error\n");
}

void bt_test_gap_request_pincode_cb(void *handle,char *device)
{
	char buffer[BUFFER_SIZE] = {0};

	btlog("AGENT:Request pincode (%s)\n",device);

	bt_test_pair_ask("Enter PIN Code: ",buffer);

	bt_manager_gap_send_pincode(handle,buffer);
}

void bt_test_gap_display_pin_code_cb(char *device,char *pincode)
{
	btlog("AGENT: Pincode %s\n", pincode);
}

void bt_test_gap_request_passkey_cb(void *handle,char *device)
{
	unsigned long passkey;
	char buffer[BUFFER_SIZE] = {0};

	btlog("AGENT: Request passkey (%s)\n",device);
	//bt_test_pair_ask("Enter passkey (1~999999): ",buffer);
	//passkey = strtoul(buffer, NULL, 10);
	//if ((passkey > 0) && (passkey < 999999))
		bt_manager_gap_send_passkey(handle,passkey);
	//else
	//	fprintf(stdout, "AGENT: get passkey error\n");
}

void bt_test_gap_display_passkey_cb(char *device,unsigned int passkey,
		unsigned int entered)
{
	btlog("AGENT: Passkey %06u\n", passkey);
}

void bt_test_gap_confirm_passkey_cb(void *handle,char *device,unsigned int passkey)
{
	char buffer[BUFFER_SIZE] = {0};

	btlog("AGENT: Request confirmation (%s)\nPasskey: %06u\n",
		device, passkey);
	//bt_test_pair_ask("Confirm passkey? (yes/no): ",buffer);
	//if (!strncmp(buffer, "yes", 3))
		bt_manager_gap_pair_send_empty_response(handle);
	//else
	//	bt_manager_gap_send_pair_error(handle,BT_PAIR_REQUEST_REJECTED,"");
}

void bt_test_gap_authorize_cb(void *handle,char *device)
{

	char buffer[BUFFER_SIZE] = {0};
	btlog("AGENT: Request authorization (%s)\n",device);

	bt_test_pair_ask("Authorize? (yes/no): ",buffer);

	//if (!strncmp(buffer, "yes", 3))
		bt_manager_gap_pair_send_empty_response(handle);
	//else
	//	bt_manager_gap_send_pair_error(handle,BT_PAIR_REQUEST_REJECTED,"");
}

void bt_test_gap_authorize_service_cb(void *handle,char *device,char *uuid)
{
	char buffer[BUFFER_SIZE] = {0};
	btlog("AGENT: Authorize Service (%s, %s)\n", device, uuid);
	//if(is_background == 0) {
	//	bt_test_pair_ask("Authorize connection? (yes/no): ",buffer);

	//	if (!strncmp(buffer, "yes", 3))
			bt_manager_gap_pair_send_empty_response(handle);
	//	else
	//		bt_manager_gap_send_pair_error(handle,BT_PAIR_REQUEST_REJECTED,"");
	//}else {
	//	bt_manager_gap_pair_send_empty_response(handle);
	//}
}

static void bt_test_a2dp_sink_connection_state_cb(const char *bd_addr, btmg_a2dp_sink_connection_state_t state)
{

	if (state == BTMG_A2DP_SINK_DISCONNECTED) {
		btlog("A2DP sink disconnected with device: %s", bd_addr);
		bt_manager_set_discovery_mode(BTMG_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
	} else if (state == BTMG_A2DP_SINK_CONNECTING) {
		btlog("A2DP sink connecting with device: %s", bd_addr);
	} else if (state == BTMG_A2DP_SINK_CONNECTED) {
		btlog("A2DP sink connected with device: %s", bd_addr);
	} else if (state == BTMG_A2DP_SINK_DISCONNECTING) {
		btlog("A2DP sink disconnecting with device: %s", bd_addr);
	}
}

static void bt_test_a2dp_sink_audio_state_cb(const char *bd_addr, btmg_a2dp_sink_audio_state_t state)
{
	if (state == BTMG_A2DP_SINK_AUDIO_SUSPENDED) {
		btlog("A2DP sink audio suspended with device: %s", bd_addr);
	} else if (state == BTMG_A2DP_SINK_AUDIO_STOPPED) {
		btlog("A2DP sink audio stopped with device: %s", bd_addr);
	} else if (state == BTMG_A2DP_SINK_AUDIO_STARTED) {
		btlog("A2DP sink audio started with device: %s", bd_addr);
	}
}

static void bt_test_a2dp_source_connection_state_cb(const char *bd_addr, btmg_a2dp_source_connection_state_t state)
{
	if (state == BTMG_A2DP_SOURCE_DISCONNECTED) {
		btlog("A2DP source disconnected with device: %s\n", bd_addr);
		bt_manager_set_discovery_mode(BTMG_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
	} else if (state == BTMG_A2DP_SOURCE_CONNECTING) {
		btlog("A2DP source connecting with device: %s\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_CONNECTED) {
		btlog("A2DP source connected with device: %s\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_DISCONNECTING) {
		btlog("A2DP source disconnecting with device: %s\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_CONNECT_FAILED) {
		btlog("A2DP source connect with device: %s failed!\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_DISCONNEC_FAILED) {
		btlog("A2DP source disconnect with device: %s failed!\n", bd_addr);
	}
}

static void bt_test_a2dp_source_audio_state_cb(const char *bd_addr, btmg_a2dp_source_audio_state_t state)
{
	if (state == BTMG_A2DP_SOURCE_AUDIO_SUSPENDED) {
		LOG_info("A2DP source audio suspended with device: %s\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_AUDIO_STOPPED) {
		LOG_info("A2DP source audio stopped with device: %s\n", bd_addr);
	} else if (state == BTMG_A2DP_SOURCE_AUDIO_STARTED) {
		LOG_info("A2DP source audio started with device: %s\n", bd_addr);
	}
}

static void bt_test_avrcp_play_state_cb(const char *bd_addr, btmg_avrcp_play_state_t state)
{
	if (state == BTMG_AVRCP_PLAYSTATE_STOPPED) {
		btlog("BT playing music stopped with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_PLAYING) {
		btlog("BT palying music playing with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_PAUSED) {
		btlog("BT palying music paused with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_FWD_SEEK) {
		btlog("BT palying music FWD SEEK with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_REV_SEEK) {
		btlog("BT palying music REV SEEK with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_FORWARD) {
		btlog("BT palying music forward with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_BACKWARD) {
		btlog("BT palying music backward with device: %s\n", bd_addr);
	} else if (state == BTMG_AVRCP_PLAYSTATE_ERROR) {
		btlog("BT palying music ERROR with device: %s\n", bd_addr);
	}
}

static void bt_test_avrcp_audio_volume_cb(const char *bd_addr, unsigned int volume)
{
	btlog("AVRCP audio volume:%s : %d\n", bd_addr, volume);
}

/////////////////////////////////

static btmg_callback_t *bt_callback = NULL;

void PLAT_bluetoothInit() {
	LOG_info("BT init\n");

	if(bt_callback) {
		LOG_error("BT is already initialized.\n");
		return;
	}
	// Needs to be set before starting bluetooth manager
	PLAT_bluetoothDiagnosticsEnable(CFG_getBluetoothDiagnostics());

	if(bt_manager_preinit(&bt_callback) != 0) {
		LOG_error("bt preinit failed!\n");
		return;
	}

	// Only BT audio here for now
	//bt_manager_set_enable_default(true);
	bt_manager_enable_profile(BTMG_A2DP_SOUCE_ENABLE | BTMG_AVRCP_ENABLE);

	bt_callback->btmg_manager_cb.bt_mg_cb = bt_test_manager_cb;
	bt_callback->btmg_adapter_cb.adapter_power_state_cb = bt_test_adapter_power_state_cb;

	bt_callback->btmg_gap_cb.gap_status_cb = bt_test_status_cb;
	bt_callback->btmg_gap_cb.gap_disc_status_cb = bt_test_discovery_status_cb;
	bt_callback->btmg_gap_cb.gap_device_add_cb = bt_test_dev_add_cb;
	bt_callback->btmg_gap_cb.gap_device_remove_cb = bt_test_dev_remove_cb;
	bt_callback->btmg_gap_cb.gap_update_rssi_cb =	bt_test_update_rssi_cb;
	bt_callback->btmg_gap_cb.gap_bond_state_cb = bt_test_bond_state_cb;
	bt_callback->btmg_gap_cb.gap_connect_changed = bt_test_gap_connected_changed_cb;

	// bt security callback setting.
	bt_callback->btmg_gap_cb.gap_request_pincode = bt_test_gap_request_pincode_cb;
	bt_callback->btmg_gap_cb.gap_display_pin_code = bt_test_gap_display_pin_code_cb;
	bt_callback->btmg_gap_cb.gap_request_passkey = bt_test_gap_request_passkey_cb;
	bt_callback->btmg_gap_cb.gap_display_passkey = bt_test_gap_display_passkey_cb;
	bt_callback->btmg_gap_cb.gap_confirm_passkey = bt_test_gap_confirm_passkey_cb;
	bt_callback->btmg_gap_cb.gap_authorize  = bt_test_gap_authorize_cb;
	bt_callback->btmg_gap_cb.gap_authorize_service = bt_test_gap_authorize_service_cb;

	// bt a2dp sink callback
	//bt_callback->btmg_a2dp_sink_cb.a2dp_sink_connection_state_cb = bt_test_a2dp_sink_connection_state_cb;
	//bt_callback->btmg_a2dp_sink_cb.a2dp_sink_audio_state_cb = bt_test_a2dp_sink_audio_state_cb;

	// bt a2dp source callback
	bt_callback->btmg_a2dp_source_cb.a2dp_source_connection_state_cb = bt_test_a2dp_source_connection_state_cb;
	bt_callback->btmg_a2dp_source_cb.a2dp_source_audio_state_cb = bt_test_a2dp_source_audio_state_cb;

	// bt avrcp callback
	bt_callback->btmg_avrcp_cb.avrcp_audio_volume_cb = bt_test_avrcp_audio_volume_cb;

	if(bt_manager_init(bt_callback) != 0) {
		LOG_error("bt manager init failed.\n");
		bt_manager_deinit(bt_callback);
		return;
	}

	bonded_devices = btmg_dev_list_new();
	if(bonded_devices == NULL) {
		LOG_error("btmg_dev_list_new failed.\n");
		bt_manager_deinit(bt_callback);
		return;
	}

	discovered_audiodev= btmg_dev_list_new();
	if(discovered_audiodev == NULL) {
		LOG_error("btmg_dev_list_new failed.\n");
		bt_manager_deinit(bt_callback);
		return;
	}

	discovered_controllers= btmg_dev_list_new();
	if(discovered_controllers == NULL) {
		LOG_error("btmg_dev_list_new failed.\n");
		bt_manager_deinit(bt_callback);
		return;
	}

	PLAT_bluetoothEnable(CFG_getBluetooth());
}

void PLAT_bluetoothDeinit()
{
	if(bt_callback) {
		bt_manager_deinit(bt_callback);
		btmg_dev_list_free(discovered_audiodev);
		btmg_dev_list_free(discovered_controllers);
		btmg_dev_list_free(bonded_devices);
		bt_callback = NULL;
	}
}

void PLAT_bluetoothEnable(bool shouldBeOn) {

	if(bt_callback) {
		// go through the manager
		btmg_state_t bt_state = bt_manager_get_state();
		// dont turn on if BT is already on/urning on or still turning off
		if(shouldBeOn && bt_state == BTMG_STATE_OFF) {		
			btlog("turning BT on...\n");
			system("rfkill.elf unblock bluetooth");
			if(bt_manager_enable(true) < 0) {
				LOG_error("bt_manager_enable failed\n");
				return;
			}
			bt_manager_set_name("Trimui Brick (NextUI)");
		}
		else if(!shouldBeOn && bt_state == BTMG_STATE_ON ) {
			btlog("turning BT off...\n");
			if(bt_manager_enable(false) < 0) {
				LOG_error("bt_manager_enable failed\n");
				return;
			}
			system("rfkill.elf block bluetooth");
		}
	}
	else {
		// lightweight
		if(shouldBeOn) {
			btlog("turning BT on...\n");
			//system("rfkill.elf unblock bluetooth");
			system("/etc/bluetooth/bt_init.sh start &");
		}
		else {
			btlog("turning BT off...\n");
			system("/etc/bluetooth/bt_init.sh stop &");
			//system("rfkill.elf block bluetooth");
		}
	}
	CFG_setBluetooth(shouldBeOn);
}

bool PLAT_bluetoothDiagnosticsEnabled() { 
	return CFG_getBluetoothDiagnostics(); 
}

void PLAT_bluetoothDiagnosticsEnable(bool on) {
	bt_manager_set_loglevel(on ? BTMG_LOG_LEVEL_DEBUG : BTMG_LOG_LEVEL_INFO);
	CFG_setBluetoothDiagnostics(on);
}

void PLAT_bluetoothDiscovery(int on)
{
	btmg_scan_filter_t scan_filter = {0};
	scan_filter.type = BTMG_SCAN_BR_EDR;
	scan_filter.rssi = -90;

	if(on) {
		btlog("Starting BT discovery.\n");
		bt_manager_discovery_filter(&scan_filter);
		bt_manager_start_discovery();
	}
	else {
		btlog("Stopping BT discovery.\n");
		bt_manager_cancel_discovery();
	}
}

bool PLAT_bluetoothDiscovering()
{
	return bt_manager_is_discovering();
}

int PLAT_bluetoothScan(struct BT_device *devices, int max)
{
	int count = 0;
	// Append audio devices
	{
		dev_node_t *dev_node = NULL;
		dev_node = discovered_audiodev->head;
		while (dev_node != NULL && count < max) {
			btlog("%s %s\n", dev_node->dev_addr, dev_node->dev_name);
			struct BT_device *device = &devices[count];
			strcpy(device->addr, dev_node->dev_addr);
			strcpy(device->name, dev_node->dev_name);
			device->kind = BLUETOOTH_AUDIO;

			count++;
			dev_node = dev_node->next;
		}
	}

	// Append controllers
	{
		dev_node_t *dev_node = NULL;
		dev_node = discovered_controllers->head;
		while (dev_node != NULL && count < max) {
			btlog("%s %s\n", dev_node->dev_addr, dev_node->dev_name);
			struct BT_device *device = &devices[count];
			strcpy(device->addr, dev_node->dev_addr);
			strcpy(device->name, dev_node->dev_name);
			device->kind = BLUETOOTH_CONTROLLER;

			count++;
			dev_node = dev_node->next;
		}
	}

	//btlog("Scan yielded %d devices\n", count);

	return count;
}

int PLAT_bluetoothPaired(struct BT_devicePaired *paired, int max)
{
	bt_paried_device *devices = NULL;
	int pairCnt = -1;

	bt_manager_get_paired_devices(&devices,&pairCnt);
	bt_paried_device *iter = devices;
	int count = 0;
	if(iter) {
		while(iter && count < max) {
			struct BT_devicePaired *device = &paired[count];
			strcpy(device->remote_addr, iter->remote_address);
			strcpy(device->remote_name, iter->remote_name);
			device->rssi = iter->rssi;
			device->is_bonded = iter->is_bonded;
			device->is_connected = iter->is_connected;
			//device->uuid_len = iter->uuid_length;

			count++;
			iter = iter->next;
		}
		bt_manager_free_paired_devices(devices);
	}
	//btlog("Paired %d devices\n", count);

	return count;
}

void PLAT_bluetoothPair(char *addr)
{
	int ret = bt_manager_pair(addr);
	if (ret)
		LOG_error("BT pair failed: %d\n", ret);
}

void PLAT_bluetoothUnpair(char *addr)
{
	int ret = bt_manager_unpair(addr);
	if (ret)
		LOG_error("BT unpair failed\n");
}

void PLAT_bluetoothConnect(char *addr)
{
	// can we get away wth just calling both?
	int ret = bt_manager_connect(addr);
	if (ret)
		LOG_error("BT connect generic failed: %d\n", ret);
	LOG_info("BT connect generic returned: %d\n", ret);
	//int ret = bt_manager_profile_connect(addr, BTMG_A2DP_SINK);
	//if (ret)
	//	LOG_error("BT connect A2DP_SINK failed: %d\n", ret);

	//ret = bt_manager_profile_connect(addr, BTMG_AVRCP);
	//if (ret)
	//	LOG_error("BT connect AVRCP failed: %d\n", ret);

	//PLAT_bluetoothStreamInit(2, 48000);
	//PLAT_bluetoothStreamBegin(0);
}

void PLAT_bluetoothDisconnect(char *addr)
{
	// can we get away wth just calling this?
	int ret = bt_manager_disconnect(addr);
	if (ret)
		LOG_error("BT disconnect failed: %d\n", ret);
	//int ret = bt_manager_profile_disconnect(addr, BTMG_A2DP_SINK);
	//if (ret)
	//	LOG_error("BT disconnect BTMG_A2DP_SINK failed: %d\n", ret);
	//ret = bt_manager_profile_disconnect(addr, BTMG_AVRCP);
	//if (ret)
	//	LOG_error("BT disconnect BTMG_AVRCP failed: %d\n", ret);
}

bool PLAT_bluetoothConnected()
{
	//bt_paried_device *devices = NULL;
	//int pairCnt = -1;
	//bool connected = false;
//
	//bt_manager_get_paired_devices(&devices,&pairCnt);
	//bt_paried_device *iter = devices;
	//if(iter) {
	//	while(iter) {
	//		if(iter->is_connected) {
	//			connected = true;
	//			break;
	//		}
//
	//		iter = iter->next;
	//	}
	//	bt_manager_free_paired_devices(devices);
	//}
	
	// no btmgr here!

	FILE *fp;
    char buffer[256];
	bool connected = false;

	fp = popen("hcitool con", "r");
    if (fp == NULL) {
        perror("Failed to run hcitool");
        return 1;
    }

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (strstr(buffer, "ACL")) {
            connected = true;
            break;
        }
    }

    pclose(fp);

	return connected;
}

int PLAT_bluetoothVolume()
{
	int vol_value = 0;

	vol_value = bt_manager_get_vol();
	btlog("get vol:%d\n", vol_value);

	return vol_value;
}

void PLAT_bluetoothSetVolume(int vol)
{
	int vol_value = vol;
	if (vol_value > 100)
		vol_value = 100;

	if (vol_value < 0)
		vol_value = 0;

	bt_manager_vol_changed_noti(vol_value);
	LOG_debug("set vol:%d\n", vol_value);
}
*/

// bt_device_watcher.c

#include <sys/inotify.h>

#define WATCHED_DIR_FMT "%s"
#define WATCHED_FILE ".asoundrc"
#define EVENT_BUF_LEN (1024 * (sizeof(struct inotify_event) + NAME_MAX + 1))

static pthread_t watcher_thread;
static int inotify_fd = -1;
static int dir_watch_fd = -1;
static int file_watch_fd = -1;
static volatile int running = 0;
static void (*callback_fn)(int device, int watch_event) = NULL;
static char watched_dir[MAX_PATH];
static char watched_file_path[MAX_PATH];

// Function to detect audio device type from .asoundrc content
static int detect_audio_device_type() {
    FILE *file = fopen(watched_file_path, "r");
    if (!file) {
		//LOG_info("detect_audio_device_type: .asoundrc not found, defaulting to AUDIO_SINK_DEFAULT\n");
        return AUDIO_SINK_DEFAULT;
    }
    
    char line[256];
    int is_bluetooth = 0;
    int is_usb_dac = 0;
    
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "type bluealsa") || strstr(line, "defaults.bluealsa.device")) {
            //LOG_info("detect_audio_device_type: found bluealsa\n");
            is_bluetooth = 1;
            break;
        }
        if (strstr(line, "type hw")) {
			//LOG_info("detect_audio_device_type: found hw card\n");
            is_usb_dac = 1;
            break;
        }
    }
    
    fclose(file);
    
    if (is_bluetooth) {
        return AUDIO_SINK_BLUETOOTH;
    } else if (is_usb_dac) {
        return AUDIO_SINK_USBDAC;
    } else {
        return AUDIO_SINK_DEFAULT;
    }
}

static void add_file_watch() {
    if (file_watch_fd >= 0) return; // already watching

    file_watch_fd = inotify_add_watch(inotify_fd, watched_file_path,
                                      IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF);
    if (file_watch_fd < 0) {
        if (errno != ENOENT) // ENOENT means file doesn't exist yet - no error needed
            LOG_error("PLAT_audioDeviceWatchRegister: failed to add file watch: %s\n", strerror(errno));
    } else {
        LOG_info("Watching file: %s\n", watched_file_path);
    }
}

static void remove_file_watch() {
    if (file_watch_fd >= 0) {
        inotify_rm_watch(inotify_fd, file_watch_fd);
        file_watch_fd = -1;
        LOG_info("Stopped watching file: %s\n", watched_file_path);
    }
}

static void *watcher_thread_func(void *arg) {
    char buffer[EVENT_BUF_LEN];

    // At start try to watch file if exists
    add_file_watch();

    while (running) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                sleep(1);
                continue;
            }
            LOG_error("inotify read error: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < length;) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            if (event->wd == dir_watch_fd) {
                if (event->len > 0 && strcmp(event->name, WATCHED_FILE) == 0) {
                    if (event->mask & IN_CREATE) {
                        add_file_watch();
                        int device_type = detect_audio_device_type();
                        if (callback_fn) callback_fn(device_type, DIRWATCH_CREATE);
                    }
					// No need to react to this, we handle it via file watch
                    //else if (event->mask & IN_DELETE) {
                    //    remove_file_watch();
                    //    if (callback_fn) callback_fn(AUDIO_SINK_DEFAULT, DIRWATCH_DELETE);
                    //}
                }
            }
            else if (event->wd == file_watch_fd) {
                if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF)) {
                    if (event->mask & IN_DELETE_SELF) {
                        remove_file_watch();
						if (callback_fn) callback_fn(AUDIO_SINK_DEFAULT, FILEWATCH_DELETE);
                    }
					// No need to react to this, it usually comes paired with FILEWATCH_MODIFY
					//else if (event->mask & IN_CLOSE_WRITE) {
					//	if (callback_fn) callback_fn(AUDIO_SINK_BLUETOOTH, FILEWATCH_CLOSE_WRITE);
					//}
					else if (event->mask & IN_MODIFY) {
						int device_type = detect_audio_device_type();
						if (callback_fn) callback_fn(device_type, FILEWATCH_MODIFY);
					}
                }
            }

            i += sizeof(struct inotify_event) + event->len;
        }
    }

    return NULL;
}

void PLAT_audioDeviceWatchRegister(void (*cb)(int device, int event)) {
    if (running) return; // Already running

    callback_fn = cb;

    const char *home = getenv("HOME");
    if (!home) {
        LOG_error("PLAT_audioDeviceWatchRegister: HOME environment variable not set\n");
        return;
    }

    snprintf(watched_dir, MAX_PATH, WATCHED_DIR_FMT, home);
    snprintf(watched_file_path, MAX_PATH, "%s/%s", watched_dir, WATCHED_FILE);

    LOG_info("PLAT_audioDeviceWatchRegister: Watching directory %s\n", watched_dir);
    LOG_info("PLAT_audioDeviceWatchRegister: Watching file %s\n", watched_file_path);

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to initialize inotify\n");
        return;
    }

    dir_watch_fd = inotify_add_watch(inotify_fd, watched_dir, IN_CREATE | IN_DELETE);
    if (dir_watch_fd < 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to add directory watch\n");
        close(inotify_fd);
        inotify_fd = -1;
        return;
    }

    file_watch_fd = -1;

    running = 1;
    if (pthread_create(&watcher_thread, NULL, watcher_thread_func, NULL) != 0) {
        LOG_error("PLAT_audioDeviceWatchRegister: failed to create thread\n");
        inotify_rm_watch(inotify_fd, dir_watch_fd);
        close(inotify_fd);
        inotify_fd = -1;
        dir_watch_fd = -1;
        running = 0;
    }
}

void PLAT_audioDeviceWatchUnregister(void) {
    if (!running) return;

    running = 0;
    pthread_join(watcher_thread, NULL);

    if (file_watch_fd >= 0)
        inotify_rm_watch(inotify_fd, file_watch_fd);
    if (dir_watch_fd >= 0)
        inotify_rm_watch(inotify_fd, dir_watch_fd);
    if (inotify_fd >= 0)
        close(inotify_fd);

    inotify_fd = -1;
    dir_watch_fd = -1;
    file_watch_fd = -1;
    callback_fn = NULL;
}

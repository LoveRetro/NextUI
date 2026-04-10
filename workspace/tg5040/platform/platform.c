// tg5040
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

int is_brick = 0;
void PLAT_initPlatform(void) {
	// TODO: replace with something that doesnt bleed out of tg5040 scope
	char *device = getenv("DEVICE");
	is_brick = exactMatch("brick", device);
}

static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;
void PLAT_initInput(void) {
	char* device = getenv("DEVICE");
	is_brick = exactMatch("brick", device);
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
	perf.cpu_temp = getInt("/sys/devices/virtual/thermal/thermal_zone0/temp")/1000;
}

void PLAT_getCPUSpeed()
{
	perf.cpu_speed = getInt("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")/1000;
}

void PLAT_getGPUTemp() {
	perf.gpu_temp = getInt("/sys/devices/virtual/thermal/thermal_zone2/temp")/1000;
}

void PLAT_getGPUSpeed() {
	perf.gpu_speed = 660; // MHz
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

void PLAT_getNetworkStatus(int* is_online)
{
	if(WIFI_enabled())
		WIFI_connectionInfo(&connection);
	else
		connection_reset(&connection);
	
	if(is_online)
		*is_online = (connection.valid && connection.ssid[0] != '\0');
	
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
		if (is_brick) SetRawBrightness(8);
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

volatile int useAutoCpu = 1;
void *PLAT_cpu_monitor(void *arg) {
    struct timespec start_time, curr_time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &start_time);

    long clock_ticks_per_sec = sysconf(_SC_CLK_TCK);

    double prev_real_time = get_time_sec();
    double prev_cpu_time = get_process_cpu_time_sec();

	// big Cortex-A53 CPUx4 - 408Mhz to 2160Mhz
	// 408000 600000 816000 1008000 1200000 1416000 1608000 1800000 2000000
	const int cpu_frequencies[] = {408,600,816,1008,1200,1416,1608,1800,2000};
    const int num_freqs = sizeof(cpu_frequencies) / sizeof(cpu_frequencies[0]);
    int current_index = 1; 

    double cpu_usage_history[ROLLING_WINDOW] = {0};
    double cpu_speed_history[ROLLING_WINDOW] = {0};
    int history_index = 0;
    int history_count = 0; 

    while (true) {

		double curr_real_time = get_time_sec();
		double curr_cpu_time = get_process_cpu_time_sec();

		double elapsed_real_time = curr_real_time - prev_real_time;
		double elapsed_cpu_time = curr_cpu_time - prev_cpu_time;

        if (useAutoCpu) {
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

            perf.cpu_usage = sum_cpu_usage / history_count;
            //perf.cpu_speed = sum_cpu_speed / history_count;

            pthread_mutex_unlock(&currentcpuinfo);

            prev_real_time = curr_real_time;
            prev_cpu_time = curr_cpu_time;
			// 20ms really seems lowest i can go, anything lower it becomes innacurate, maybe one day I will find another even more granual way to calculate usage accurately and lower this shit to 1ms haha, altough anything lower than 10ms causes cpu usage in itself so yeah
			// Anyways screw it 20ms is pretty much on a frame by frame basis anyways, so will anything lower really make a difference specially if that introduces cpu usage by itself? 
			// Who knows, maybe some CPU engineer will find my comment here one day and can explain, maybe this is looking for the limits of C and needs Assambler or whatever to call CPU instructions directly to go further, but all I know is PUSH and MOV, how did the orignal Roller Coaster Tycoon developer wrote a whole game like this anyways? Its insane..
            usleep(20000);

        } else {
			// Just measure CPU usage without changing frequency

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

                perf.cpu_usage = sum_cpu_usage / history_count;

                pthread_mutex_unlock(&currentcpuinfo);
            }

            prev_real_time = curr_real_time;
            prev_cpu_time = curr_cpu_time;
            usleep(100000); 
        }
    }
}


#define GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed"
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
		case CPU_SPEED_MENU: 		freq =  600000; perf.cpu_speed = 600; break;
		case CPU_SPEED_POWERSAVE:	freq = 1200000; perf.cpu_speed = 1200; break;
		case CPU_SPEED_NORMAL: 		freq = 1608000; perf.cpu_speed = 1600; break;
		case CPU_SPEED_PERFORMANCE: freq = 2000000; perf.cpu_speed = 2000; break;
	}
	putInt(GOVERNOR_PATH, freq);
}

#define MAX_STRENGTH 0xFFFF
#define MIN_VOLTAGE 500000
#define MAX_VOLTAGE 3300000
#define RUMBLE_PATH "/sys/class/gpio/gpio227/value"
#define RUMBLE_VOLTAGE_PATH "/sys/class/motor/voltage"

void PLAT_setRumble(int strength) {
	int voltage = MAX_VOLTAGE;

	if(strength > 0 && strength < MAX_STRENGTH) {
		voltage = MIN_VOLTAGE + (int)(strength * ((long long)(MAX_VOLTAGE - MIN_VOLTAGE) / MAX_STRENGTH));
		putInt(RUMBLE_VOLTAGE_PATH, voltage);
	}
	else {
		putInt(RUMBLE_VOLTAGE_PATH, MAX_VOLTAGE);
	}

	// enable rumble - removed the FN switch disabling haptics
	// did not make sense 
	putInt(RUMBLE_PATH, (strength) ? 1 : 0);
}

int PLAT_pickSampleRate(int requested, int max) {
	// bluetooth: allow limiting the maximum to improve compatibility
	if(PLAT_bluetoothConnected())
		return MIN(requested, CFG_getBluetoothSamplingrateLimit());

	return MIN(requested, max);
}

void PLAT_overrideMute(int mute) {
	putInt("/sys/class/speaker/mute", mute);
}

char* PLAT_getModel(void) {
	char* model = getenv("TRIMUI_MODEL");
	if (model) return model;
	return "Trimui Smart Pro";
}

void PLAT_getOsVersionInfo(char* output_str, size_t max_len)
{
	return getFile("/etc/version", output_str,max_len);
}

bool PLAT_btIsConnected(void)
{
	return bluetoothConnected;
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
	char* device = getenv("DEVICE");
	is_brick = exactMatch("brick", device);
	if(is_brick) {
	lightsDefault[0] = (LightSettings) {
		"FN 1 key",
		"f1",
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
		"FN 2 key",
		"f2",
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
		"Topbar",
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
	lightsDefault[3] = (LightSettings) {
		"L/R triggers",
		"lr",
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
} else {
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
}
void PLAT_initLeds(LightSettings *lights) 
{
	char* device = getenv("DEVICE");
	is_brick = exactMatch("brick", device);

	PLAT_initDefaultLeds();
	FILE *file;
	if(is_brick) {
		file = PLAT_OpenSettings("ledsettings_brick.txt");
	}
	else {
		file = PLAT_OpenSettings("ledsettings.txt");
	}

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
	if(is_brick) {
		if (strcmp(led->filename, "m") == 0) {
			snprintf(filepath, sizeof(filepath), LED_PATH1);
		} else if (strcmp(led->filename, "f1") == 0) {
			snprintf(filepath, sizeof(filepath),LED_PATH3);
		} else  {
			snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale_%s", led->filename);
		}
	} else {
		snprintf(filepath, sizeof(filepath), LED_PATH1);
	}
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
	if(is_brick) {
		if (strcmp(led->filename, "m") == 0) {
			snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale");
		} else if (strcmp(led->filename, "f1") == 0) {
			snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale_f1f2");
		} else  {
			snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale_%s", led->filename);
		}
	} else {
		snprintf(filepath, sizeof(filepath), "/sys/class/led_anim/max_scale");
	}
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
#define WIFI_SOCK_DIR "/etc/wifi/sockets"
#include "generic_wifi.c"

/////////////////////////

// We use the generic bluetooth implementation here
#include "generic_bt.c"

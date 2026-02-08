// my355
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>
#include <tinyalsa/mixer.h>

#include "msettings.h"

///////////////////////////////////////

typedef struct SettingsV1 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
	int hdmi;
	int audiosink; // was bluetooth true/false before
} SettingsV1;

// When incrementing SETTINGS_VERSION, update the Settings typedef and add
// backwards compatibility to InitSettings!
#define SETTINGS_VERSION 1
typedef SettingsV1 Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = SETTINGS_DEFAULT_BRIGHTNESS,
	.colortemperature = SETTINGS_DEFAULT_COLORTEMP,
	.headphones = SETTINGS_DEFAULT_HEADPHONE_VOLUME,
	.speaker = SETTINGS_DEFAULT_VOLUME,
	.mute = 0,
	.contrast = SETTINGS_DEFAULT_CONTRAST,
	.saturation = SETTINGS_DEFAULT_SATURATION,
	.exposure = SETTINGS_DEFAULT_EXPOSURE,
	.jack = 0,
	.hdmi = 0,
	.audiosink = AUDIO_SINK_DEFAULT,
};
static Settings* settings;

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];
static int shm_fd = -1;
static int is_host = 0;
static int shm_size = sizeof(Settings);

int scaleBrightness(int);
int scaleColortemp(int);
int scaleContrast(int);
int scaleSaturation(int);
int scaleExposure(int);
int scaleVolume(int);

void disableDpad(int);
void emulateJoystick(int);
void turboA(int);
void turboB(int);
void turboX(int);
void turboY(int);
void turboL1(int);
void turboL2(int);
void turboR1(int);
void turboR2(int);

int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}
void getFile(char* path, char* buffer, size_t buffer_size) {
	FILE *file = fopen(path, "r");
	if (file) {
		fseek(file, 0L, SEEK_END);
		size_t size = ftell(file);
		if (size>buffer_size-1) size = buffer_size - 1;
		rewind(file);
		fread(buffer, sizeof(char), size, file);
		fclose(file);
		buffer[size] = '\0';
	}
}
void putFile(char* path, char* contents) {
	FILE* file = fopen(path, "w");
	if (file) {
		fputs(contents, file);
		fclose(file);
	}
}
void putInt(char* path, int value) {
	char buffer[8];
	sprintf(buffer, "%d", value);
	putFile(path, buffer);
}

void touch(char* path) {
	close(open(path, O_RDWR|O_CREAT, 0777));
}

int exactMatch(char* str1, char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

int peekVersion(const char *filename) {
	int version = 0;
	FILE *file = fopen(filename, "r");
	if (file) {
		fread(&version, sizeof(int), 1, file);
		fclose(file);
	}
	return version;
}

#define JACK_STATE_PATH "/sys/class/gpio/gpio150/value"
#define HDMI_STATE_PATH "/sys/class/drm/card0-HDMI-A-1/status"

static int JACK_enabled(void) {
	return !getInt(JACK_STATE_PATH);
}
static int HDMI_enabled(void) {
	char value[64];
	getFile(HDMI_STATE_PATH, value, 64);
	return exactMatch(value, "connected\n");
}

void InitSettings(void) {
	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));

	shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	else { // host
		puts("Settings host"); // should always be keymon
		is_host = 1;
		// we created it so set initial size and populate
		ftruncate(shm_fd, shm_size);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

		// peek the first int from fd, it's the version
		int version = peekVersion(SettingsPath);
		if(version > 0) {
			int fd = open(SettingsPath, O_RDONLY);
			if (fd>=0) {
				if (version == SETTINGS_VERSION) {
					read(fd, settings, shm_size);
				}
				else {
					// initialize with defaults
					memcpy(settings, &DefaultSettings, shm_size);

					// overwrite with migrated data
					if(version==42) {
						// do migration (TODO when needed)
					}
					else {
						printf("Found unsupported settings version: %i.\n", version);
					}
				}

				close(fd);
			}
			else {
				// load defaults
				memcpy(settings, &DefaultSettings, shm_size);
			}
		}
		else {
			// load defaults
			memcpy(settings, &DefaultSettings, shm_size);
		}

		// these shouldn't be persisted
		// settings->jack = 0;
		// settings->hdmi = 0;
	}

	int jack = JACK_enabled();
	int hdmi = HDMI_enabled();
	printf("brightness: %i (hdmi: %i)\nspeaker: %i (jack: %i)\n", settings->brightness, hdmi, settings->speaker, jack); fflush(stdout);

	// // both of these set volume
	SetJack(jack);
	SetHDMI(hdmi);

	char cmd[256];
	sprintf(cmd, "amixer sset 'Playback Path' '%s' > /dev/null 2>&1", GetJack() ? "HP" : "SPK");
	system(cmd);

	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
	// system("echo $(< " BRIGHTNESS_PATH ")");
}
int InitializedSettings(void) {
	return (settings != NULL);
}
void QuitSettings(void) {
	munmap(settings, shm_size);
	if (is_host) shm_unlink(SHM_KEY);
}
static inline void SaveSettings(void) {
	int fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
	if (fd>=0) {
		write(fd, settings, shm_size);
		close(fd);
		sync();
	}
}

///////// Getters exposed in public API

int GetBrightness(void) { // 0-10
	return settings->brightness;
}
int GetColortemp(void) { // 0-10
	return settings->colortemperature;
}
int GetVolume(void) { // 0-20
	if(settings->jack || settings->audiosink != AUDIO_SINK_DEFAULT)
		return settings->headphones;

	return settings->speaker;
}
// monitored and set by thread in keymon
int GetJack(void) {
	return settings->jack;
}
// monitored and set by thread in audiomon
int GetAudioSink(void) {
	return settings->audiosink;
}

int GetHDMI(void) {
	// printf("GetHDMI() %i\n", settings->hdmi); fflush(stdout);
	return settings->hdmi;
};

int GetMute(void) { 
	return 0; 
}

int GetContrast(void)
{
	return settings->contrast;
}
int GetSaturation(void)
{
	return settings->saturation;
}
int GetExposure(void)
{
	return settings->exposure;
}
int GetMutedBrightness(void)
{
	return 0;
}
int GetMutedColortemp(void)
{
	return 0;
}
int GetMutedContrast(void)
{
	return 0;
}
int GetMutedSaturation(void)
{
	return 0;
}
int GetMutedExposure(void)
{
	return 0;
}
int GetMutedVolume(void)
{
	return 0;
}
int GetMuteDisablesDpad(void)
{
	return 0;
}
int GetMuteEmulatesJoystick(void)
{
	return 0;
}
int GetMuteTurboA(void)
{
	return 0;
}
int GetMuteTurboB(void)
{
	return 0;
}
int GetMuteTurboX(void)
{
	return 0;
}
int GetMuteTurboY(void)
{
	return 0;
}
int GetMuteTurboL1(void)
{
	return 0;
}
int GetMuteTurboL2(void)
{
	return 0;
}
int GetMuteTurboR1(void)
{
	return 0;
}
int GetMuteTurboR2(void)
{
	return 0;
}

///////// Setters exposed in public API

void SetBrightness(int value) {
	SetRawBrightness(scaleBrightness(value));
	settings->brightness = value;
	SaveSettings();
}
void SetColortemp(int value) {
	SetRawColortemp(scaleColortemp(value));
	settings->colortemperature = value;
	SaveSettings();
}
void SetVolume(int value) { // 0-20
	if (settings->jack || settings->audiosink != AUDIO_SINK_DEFAULT)
		settings->headphones = value;
	else
		settings->speaker = value;

	SetRawVolume(scaleVolume(value));
	SaveSettings();
}
// monitored and set by thread in keymon
void SetJack(int value) {
	printf("SetJack(%i)\n", value); fflush(stdout);

	settings->jack = value;
	SetVolume(GetVolume());
}
// monitored and set by thread in audiomon
void SetAudioSink(int value) {
	printf("SetAudioSink(%i)\n", value); fflush(stdout);

	settings->audiosink = value;
	SetVolume(GetVolume());
}

void SetHDMI(int value){
	// printf("SetHDMI(%i)\n", value); fflush(stdout);
	settings->hdmi = value;
	if (value) SetRawVolume(100); // max
	else SetVolume(GetVolume()); // restore
};

void SetMute(int value){}

void SetContrast(int value)
{
	SetRawContrast(scaleContrast(value));
	settings->contrast = value;
	SaveSettings();
}
void SetSaturation(int value)
{
	SetRawSaturation(scaleSaturation(value));
	settings->saturation = value;
	SaveSettings();
}
void SetExposure(int value)
{
	SetRawExposure(scaleExposure(value));
	settings->exposure = value;
	SaveSettings();
}

void SetMutedBrightness(int value)
{
}

void SetMutedColortemp(int value)
{
}

void SetMutedContrast(int value)
{
}

void SetMutedSaturation(int value)
{
}

void SetMutedExposure(int value)
{
}

void SetMutedVolume(int value)
{
}

void SetMuteDisablesDpad(int value)
{
}
void SetMuteEmulatesJoystick(int value)
{
}

void SetMuteTurboA(int value)
{
}

void SetMuteTurboB(int value)
{
}

void SetMuteTurboX(int value)
{
}

void SetMuteTurboY(int value)
{
}

void SetMuteTurboL1(int value)
{
}

void SetMuteTurboL2(int value)
{
}

void SetMuteTurboR1(int value)
{
}

void SetMuteTurboR2(int value)
{
}

///////// Platform specific scaling

int scaleVolume(int value) {
	return value * 5; // scale 0-20 to 0-100
}

int scaleBrightness(int value) {
	int raw;
	switch (value) {
		// TODO :revisit
		case  0: raw =   1; break;	// 
		case  1: raw =   6; break;	// 
		case  2: raw =  10; break;	// 
		case  3: raw =  16; break;	// 
		case  4: raw =  32; break;	// 
		case  5: raw =  48; break;	// 
		case  6: raw =  64; break;	// 
		case  7: raw =  96; break;	// 
		case  8: raw = 128; break;	// 
		case  9: raw = 192; break;	// 
		case 10: raw = 255; break;	// 
	}
	return raw;
}
int scaleColortemp(int value) {
	int raw;

	switch (value) {
		case 0: raw=-200; break; 		// 8
		case 1: raw=-190; break; 		// 8
		case 2: raw=-180; break; 		// 16
		case 3: raw=-170; break;		// 16
		case 4: raw=-160; break;		// 24
		case 5: raw=-150; break;		// 24
		case 6: raw=-140; break;		// 32
		case 7: raw=-130; break;		// 32
		case 8: raw=-120; break;		// 32
		case 9: raw=-110; break;	// 64
		case 10: raw=-100; break; 		// 0
		case 11: raw=-90; break; 		// 8
		case 12: raw=-80; break; 		// 8
		case 13: raw=-70; break; 		// 16
		case 14: raw=-60; break;		// 16
		case 15: raw=-50; break;		// 24
		case 16: raw=-40; break;		// 24
		case 17: raw=-30; break;		// 32
		case 18: raw=-20; break;		// 32
		case 19: raw=-10; break;		// 32
		case 20: raw=0; break;	// 64
		case 21: raw=10; break; 		// 0
		case 22: raw=20; break; 		// 8
		case 23: raw=30; break; 		// 8
		case 24: raw=40; break; 		// 16
		case 25: raw=50; break;		// 16
		case 26: raw=60; break;		// 24
		case 27: raw=70; break;		// 24
		case 28: raw=80; break;		// 32
		case 29: raw=90; break;		// 32
		case 30: raw=100; break;		// 32
		case 31: raw=110; break;	// 64
		case 32: raw=120; break; 		// 0
		case 33: raw=130; break; 		// 8
		case 34: raw=140; break; 		// 8
		case 35: raw=150; break; 		// 16
		case 36: raw=160; break;		// 16
		case 37: raw=170; break;		// 24
		case 38: raw=180; break;		// 24
		case 39: raw=190; break;		// 32
		case 40: raw=200; break;		// 32
	}
	return raw;
}
int scaleContrast(int value) {
	int raw;

	switch (value) {
		// dont offer -5/ raw 0, looks like it might turn off the display completely?
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}
int scaleSaturation(int value) {
	int raw;

	switch (value) {
		case -5: raw=0; break;
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}
int scaleExposure(int value) {
	int raw;

	switch (value) {
		// stock OS also avoids setting anything lower, so we do the same here.
		case -4: raw=10; break;
		case -3: raw=20; break;
		case -2: raw=30; break;
		case -1: raw=40; break;
		case 0: raw=50; break;
		case 1: raw=60; break;
		case 2: raw=70; break;
		case 3: raw=80; break;
		case 4: raw=90; break;
		case 5: raw=100; break;
	}
	return raw;
}

///////// Platform specific, unscaled accessors

void SetRawBrightness(int val) { // 0 - 255
	printf("SetRawBrightness(%i)\n", val); fflush(stdout);
	putInt("/sys/class/backlight/backlight/brightness", val);
}

void SetRawColortemp(int val) { // 0 - 255
	printf("SetRawColortemp(%i)\n", val); fflush(stdout);

	FILE *fd = fopen("/sys/class/disp/disp/attr/color_temperature", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}

// Find the first A2DP playback volume control via amixer
static int get_a2dp_simple_control_name(char *buf, size_t buflen) {
    FILE *fp = popen("amixer scontrols", "r");
    if (!fp) return 0;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *start = strchr(line, '\'');
        char *end = strrchr(line, '\'');
        if (start && end && end > start) {
            size_t len = end - start - 1;
            if (len < buflen) {
                strncpy(buf, start + 1, len);
                buf[len] = '\0';
                if (strstr(buf, "A2DP")) { // first A2DP simple control
                    pclose(fp);
					char esc_buf[128];
					char *src = buf;
					char *dst = esc_buf;
					while(*src && (dst - esc_buf) < (sizeof(esc_buf) - 4)) {
						if(*src == '\"') {
							*dst++ = '\\';
							*dst++ = '\"';
						} else {
							*dst++ = *src;
						}
						src++;
					}
					*dst = '\0';
					strncpy(buf, esc_buf, buflen);
					buf[buflen - 1] = '\0';
					return 1;
                }
            }
        }
    }

    pclose(fp);
    return 0;
}

void SetRawVolume(int val) { // 0-100

    if (GetAudioSink() == AUDIO_SINK_BLUETOOTH) {
        // bluealsa is a mixer plugin, not exposed as a separate card
        char ctl_name[128] = {0};
        if (get_a2dp_simple_control_name(ctl_name, sizeof(ctl_name))) {
			char cmd[256];
            // Update volume on the device
            snprintf(cmd, sizeof(cmd), "amixer sset \"%s\" -M %d%% &> /dev/null", ctl_name, val);
            system(cmd);
			//printf("Set '%s' to %d%%\n", ctl_name, val); fflush(stdout);
		}
    } 
	else if (GetAudioSink() == AUDIO_SINK_USBDAC) {
		// USB DAC path: use card 1
		struct mixer *mixer = mixer_open(1);
		if (!mixer) {
			printf("Failed to open mixer\n"); fflush(stdout);
			return;
		}

        const unsigned int num_controls = mixer_get_num_ctls(mixer);
        for (unsigned int i = 0; i < num_controls; i++) {
            struct mixer_ctl *ctl = mixer_get_ctl(mixer, i);
            const char *name = mixer_ctl_get_name(ctl);
            if (!name) continue;

            if (strstr(name, "PCM") && (strstr(name, "Volume") || strstr(name, "volume"))) {
                if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_INT) {
                    int min = mixer_ctl_get_range_min(ctl);
                    int max = mixer_ctl_get_range_max(ctl);
                    int volume = min + (val * (max - min)) / 100;
					unsigned int num_values = mixer_ctl_get_num_values(ctl);
					for (unsigned int i = 0; i < num_values; i++)
						mixer_ctl_set_value(ctl, i, volume);
                }
                break;
            }
        }
		mixer_close(mixer);
	}
	else {
		system("amixer sset 'SPK' 1% > /dev/null 2>&1"); // ensure there is always a change
		if (settings->jack) {
			system("amixer sset 'Playback Path' 'HP' > /dev/null 2>&1");
			puts("headphones"); fflush(stdout);
		}
		else if (val==0) {
			system("amixer sset 'Playback Path' 'OFF' > /dev/null 2>&1"); // mute speaker (not headphone as that produces white noise)
			puts("mute"); fflush(stdout);
		}
		else {
			system("amixer sset 'Playback Path' 'SPK' > /dev/null 2>&1");
			puts("speaker"); fflush(stdout);
		}
		
		char cmd[256];
		sprintf(cmd, "amixer sset 'SPK' %i%% > /dev/null 2>&1", val);
		puts(cmd); fflush(stdout);
		system(cmd);
	}
}

void SetRawContrast(int val){
	printf("SetRawContrast(%i)\n", val); fflush(stdout);

	FILE *fd = fopen("/sys/class/disp/disp/attr/enhance_contrast", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawSaturation(int val){
	printf("SetRawSaturation(%i)\n", val); fflush(stdout);

	FILE *fd = fopen("/sys/class/disp/disp/attr/enhance_saturation", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}
void SetRawExposure(int val){
	printf("SetRawExposure(%i)\n", val); fflush(stdout);

	FILE *fd = fopen("/sys/class/disp/disp/attr/enhance_bright", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}

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
#include <linux/i2c-dev.h>
#include <tinyalsa/mixer.h>

#include "msettings.h"

///////////////////////////////////////

typedef struct SettingsV1 {
	int version; // future proofing
	int brightness;
	int headphones;
	int speaker;
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
	.headphones = SETTINGS_DEFAULT_HEADPHONE_VOLUME,
	.speaker = SETTINGS_DEFAULT_VOLUME,
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
	char buffer[16]; // ns periods don't fit in 8
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
static int JACK_enabled(void) {
	return !getInt(JACK_STATE_PATH);
}

#define HDMI_STATE_PATH "/sys/class/drm/card0-HDMI-A-1/status"
static int HDMI_enabled(void) {
	char value[64];
	getFile(HDMI_STATE_PATH, value, 64);
	return exactMatch(value, "connected\n");
}

// Number of the first card in /proc/asound/cards matching needle, or -1.
static int get_card_num(const char* needle) {
	FILE *fp = fopen("/proc/asound/cards", "r");
	if (!fp) return -1;

	char line[256];
	int card_num = -1;
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, needle) == NULL) continue;
		int num;
		if (sscanf(line, " %d ", &num) == 1) {
			card_num = num;
			break;
		}
	}

	fclose(fp);
	return card_num;
}

static int get_usbc_card_num() {
	// can't just skip the codec, there's always a "rockchiphdmi" card too
	return get_card_num("USB-Audio");
}

static int get_rockchip_card_num() {
	return get_card_num("rockchiprk817"); // the built-in codec
}

// only while the built-in codec is the sink, so audiomon's BT/USB routing wins
static void route_audio_to_hdmi(int enable) {
	if (settings->audiosink != AUDIO_SINK_DEFAULT) return;

	char path[512];
	snprintf(path, sizeof(path), "%s/.asoundrc", getenv("USERDATA_PATH"));

	if (!enable) {
		unlink(path);
		return;
	}

	int card = get_card_num("rockchiphdmi");
	if (card < 0) {
		printf("SetHDMI: no HDMI audio card\n");
		return;
	}

	FILE *fp = fopen(path, "w");
	if (!fp) return;
	fprintf(fp,
		"pcm.!default {\n    type hw\n    card %i\n}\n"
		"ctl.!default {\n    type hw\n    card %i\n}\n", card, card);
	fflush(fp);
	fsync(fileno(fp));
	fclose(fp);
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

		// persisted with the rest of the struct but audiomon owns it at runtime
		settings->audiosink = AUDIO_SINK_DEFAULT;
	}

	// Always re-set Jack and HDMI according to hardware state
	settings->jack = JACK_enabled();
	settings->hdmi = HDMI_enabled();
	route_audio_to_hdmi(settings->hdmi); // per app start, launch.sh clears it at boot

	printf("brightness: %i (hdmi: %i)\nspeaker: %i (jack: %i)\n", settings->brightness, settings->hdmi, settings->speaker, settings->jack); 
	fflush(stdout);
	system("amixer");

	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
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
	return 0; // not supported on this device
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
int GetDisplayCalEnabled(void)
{
	// my355 does not currently expose a display gamma/LUT control interface.
	return 0;
}
int GetDisplayCalRedGain(void)
{
	return 100;
}
int GetDisplayCalGreenGain(void)
{
	return 100;
}
int GetDisplayCalBlueGain(void)
{
	return 100;
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
void SetColortemp(int value) {}
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
	if (value == AUDIO_SINK_DEFAULT) route_audio_to_hdmi(settings->hdmi);
	SetVolume(GetVolume());
}

void SetHDMI(int value){
	printf("SetHDMI(%i)\n", value); fflush(stdout);

	settings->hdmi = value;
	route_audio_to_hdmi(value);
	SetBrightness(GetBrightness()); // the panel is only blanked while docked
	// volume on HDMI belongs to the sink; the rk817 mixer doesn't reach that card
	if (!value) SetVolume(GetVolume());
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
void SetDisplayCalEnabled(int value)
{
	(void)value;
}
void SetDisplayCalRedGain(int value)
{
	(void)value;
}
void SetDisplayCalGreenGain(int value)
{
	(void)value;
}
void SetDisplayCalBlueGain(int value)
{
	(void)value;
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
	// even 1.48x steps, floored where the backlight still lights
	switch (value) {
		case  0: raw =   5; break;	//   2.0%
		case  1: raw =   7; break;	//   2.7%
		case  2: raw =  11; break;	//   4.3%
		case  3: raw =  16; break;	//   6.3%
		case  4: raw =  24; break;	//   9.4%
		case  5: raw =  36; break;	//  14.1%
		case  6: raw =  53; break;	//  20.8%
		case  7: raw =  78; break;	//  30.6%
		case  8: raw = 116; break;	//  45.5%
		case  9: raw = 172; break;	//  67.5%
		case 10: raw = 255; break;	// 100.0%
		default: raw = 255; break;
	}
	return raw;
}
int scaleColortemp(int value) {
	return 0; // not supported on this device
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
	return 0; // not supported on this device
}

///////// Platform specific, unscaled accessors

#define BL_PWM "/sys/class/pwm/pwmchip0/pwm0" // launch.sh takes it from pwm-backlight
#define BL_PERIOD 1250000 // ns, the 800Hz the driver used

void SetRawBrightness(int val) { // 0 - 255
	if (settings->hdmi) val = 0; // panel is unused while docked
	printf("SetRawBrightness(%i)\n", val); fflush(stdout);

	// all three every time, so the channel recovers if it loses state
	putInt(BL_PWM "/period", BL_PERIOD);
	putInt(BL_PWM "/duty_cycle", val * BL_PERIOD / 255);
	putInt(BL_PWM "/enable", val > 0);
}

void SetRawColortemp(int val) { // 0 - 100
	// not supported on this device, so do nothing
}

#define PANEL_CONNECTOR_ID 179 // DSI-1, the internal panel; HDMI-A-1 is 164

static void set_panel_property(const char* name, int val) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "modetest -M rockchip -w %d:%s:%d", PANEL_CONNECTOR_ID, name, val);
	system(cmd);
}

void SetRawContrast(int val){
	printf("SetRawContrast(%i)\n", val); fflush(stdout);
	set_panel_property("contrast", val);
}
void SetRawSaturation(int val){
	printf("SetRawSaturation(%i)\n", val); fflush(stdout);
	set_panel_property("saturation", val);
}
void SetRawExposure(int val){
	// not supported on this device, so do nothing
}
void SetRawDisplayCal(int enabled, int red_gain, int green_gain, int blue_gain) {
	(void)enabled;
	(void)red_gain;
	(void)green_gain;
	(void)blue_gain;
}

// "SPK Volume" only reaches -37.5dB of the codec's -95dB range, so the bottom of
// the curve goes straight to its DAC volume registers.
#define RK817_I2C_DEV "/dev/i2c-0"
#define RK817_I2C_ADDR 0x20
#define RK817_DDAC_VOLL 0x31
#define RK817_DDAC_VOLR 0x32
#define RK817_MIXER_MAX 100 // most attenuation "SPK Volume" can express
#define RK817_VOL_STEPS 8 // per volume level, so 3dB a step and -57dB at level 1

static void set_rk817_dac_volume(int att) { // 0-255, 0.375dB a step, 0 is 0dB
	int fd = open(RK817_I2C_DEV, O_RDWR);
	if (fd < 0) return;

	if (ioctl(fd, I2C_SLAVE_FORCE, RK817_I2C_ADDR) >= 0) { // the driver holds it
		unsigned char buf[2] = { RK817_DDAC_VOLL, att };
		write(fd, buf, sizeof(buf));
		buf[0] = RK817_DDAC_VOLR;
		write(fd, buf, sizeof(buf));
	}
	close(fd);
}

// Find the first playback volume control via amixer.
static int get_a2dp_simple_control_name(char *buf, size_t buflen) {
    FILE *fp = popen("amixer scontents", "r");
    if (!fp) return 0;

    char line[256];
    char name[128] = {0};
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *start = strchr(line, '\'');
        char *end = strrchr(line, '\'');
        if (start && end && end > start) { // "Simple mixer control 'NAME',0"
            size_t len = end - start - 1;
            if (len < sizeof(name)) {
                strncpy(name, start + 1, len);
                name[len] = '\0';
            }
            continue;
        }
        if (name[0] && strstr(line, "Capabilities:") && strstr(line, "volume")) {
            found = 1;
            break;
        }
    }
    pclose(fp);
    if (!found) return 0;

    char esc_buf[128];
    char *src = name;
    char *dst = esc_buf;
    while (*src && (dst - esc_buf) < (int)(sizeof(esc_buf) - 4)) {
        if (*src == '"') *dst++ = '\\';
        *dst++ = *src++;
    }
    *dst = '\0';
    strncpy(buf, esc_buf, buflen);
    buf[buflen - 1] = '\0';
    return 1;
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
		// USB DAC path: grab the first card that is not called "rockchiprk817"
		int card_num = get_usbc_card_num();
		if(card_num < 0) {
			printf("Failed to find USB audio card\n"); fflush(stdout);
			return;
		}

		struct mixer *mixer = mixer_open(card_num);
		if (!mixer) {
			printf("Failed to open mixer\n"); fflush(stdout);
			return;
		}

		// USB DACs don't agree on control names, so drive every playback one
		const unsigned int num_controls = mixer_get_num_ctls(mixer);
		for (unsigned int i = 0; i < num_controls; i++) {
			struct mixer_ctl *ctl = mixer_get_ctl(mixer, i);
			const char *name = mixer_ctl_get_name(ctl);
			if (!name || strstr(name, "Capture")) continue;

			enum mixer_ctl_type type = mixer_ctl_get_type(ctl);
			unsigned int num_values = mixer_ctl_get_num_values(ctl);

			if (strstr(name, "Volume") && type == MIXER_CTL_TYPE_INT) {
				int min = mixer_ctl_get_range_min(ctl);
				int max = mixer_ctl_get_range_max(ctl);
				int volume = min + (val * (max - min)) / 100;
				for (unsigned int j = 0; j < num_values; j++)
					mixer_ctl_set_value(ctl, j, volume);
			}
			// the lowest step is usually still audible, mute for real at 0
			else if (strstr(name, "Switch") && type == MIXER_CTL_TYPE_BOOL) {
				for (unsigned int j = 0; j < num_values; j++)
					mixer_ctl_set_value(ctl, j, val > 0);
			}
		}
		mixer_close(mixer);
	}
	else {
		// Speaker path: grab the card that is called "rockchiprk817"
		int card_num = get_rockchip_card_num();
		if(card_num < 0) {
			card_num = 0; // fallback to card 0 if we can't find it
		}

		struct mixer *mixer = mixer_open(card_num);
		if (!mixer) {
			printf("Failed to open mixer\n"); fflush(stdout);
			return;
		}

		struct mixer_ctl *path = mixer_get_ctl_by_name(mixer, "Playback Path");
		if (path) {
			// I don't seem to have white noise with headphones off
			if (val == 0) mixer_ctl_set_enum_by_string(path, "OFF");
			else if (settings->jack) mixer_ctl_set_enum_by_string(path, "HP");
			else mixer_ctl_set_enum_by_string(path, "SPK");
		}

		// Attempt to mute the individual switches but I am not sure it does anything
		struct mixer_ctl *spk_sw = mixer_get_ctl_by_name(mixer, "Speaker Switch");
		if (spk_sw) mixer_ctl_set_value(spk_sw, 0, (!settings->jack && val > 0));

		struct mixer_ctl *hp_sw = mixer_get_ctl_by_name(mixer, "Headphone Switch");
		if (hp_sw) mixer_ctl_set_value(hp_sw, 0, (settings->jack && val > 0));

		int att = ((100 - val) * RK817_VOL_STEPS) / 5;

		struct mixer_ctl *vol = mixer_get_ctl_by_name(mixer, "SPK Volume");
		if (vol) {
			int capped = att > RK817_MIXER_MAX ? RK817_MIXER_MAX : att;
			unsigned int num_values = mixer_ctl_get_num_values(vol);
			for (unsigned int i = 0; i < num_values; i++) {
				mixer_ctl_set_value(vol, i, RK817_MIXER_MAX - capped);
			}
		}

		mixer_close(mixer);

		// after Playback Path, which resets it, and unconditional because ALSA
		// skips writes that leave the control's value unchanged
		set_rk817_dac_volume(att);
	}
}

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
/* #include <tinyalsa/mixer.h> */ // Needed?

#include "msettings.h"

///////////////////////////////////////

// Legacy MinUI settings
typedef struct SettingsV3 {
	int version; // future proofing
	int brightness;
	int headphones;
	int speaker;
	int mute;
	int unused[2];
	int jack;
} SettingsV3;

// First NextUI settings format
typedef struct SettingsV4 {
	int version; // future proofing
	int brightness;
	int colortemperature; // 0-20
	int headphones;
	int speaker;
	int mute;
	int unused[2];
	int jack;
} SettingsV4;

// Second NextUI settings format
typedef struct SettingsV5 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
} SettingsV5;

// Third NextUI settings format
typedef struct SettingsV6 {
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
} SettingsV6;

typedef struct SettingsV7 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int mutedbrightness;
	int mutedcolortemperature;
	int mutedcontrast;
	int mutedsaturation;
	int mutedexposure;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
} SettingsV7;

typedef struct SettingsV8 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int toggled_brightness;
	int toggled_colortemperature;
	int toggled_contrast;
	int toggled_saturation;
	int toggled_exposure;
	int toggled_volume;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
	int hdmi;
} SettingsV8;

typedef struct SettingsV9 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int toggled_brightness;
	int toggled_colortemperature;
	int toggled_contrast;
	int toggled_saturation;
	int toggled_exposure;
	int toggled_volume;
	int disable_dpad_on_mute;
	int emulate_joystick_on_mute;
	int turbo_a;
	int turbo_b;
	int turbo_x;
	int turbo_y;
	int turbo_l1;
	int turbo_l2;
	int turbo_r1;
	int turbo_r2;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
} SettingsV9;

typedef struct SettingsV10 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int mute;
	int contrast;
	int saturation;
	int exposure;
	int toggled_brightness;
	int toggled_colortemperature;
	int toggled_contrast;
	int toggled_saturation;
	int toggled_exposure;
	int toggled_volume;
	int disable_dpad_on_mute;
	int emulate_joystick_on_mute;
	int turbo_a;
	int turbo_b;
	int turbo_x;
	int turbo_y;
	int turbo_l1;
	int turbo_l2;
	int turbo_r1;
	int turbo_r2;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
	int audiosink; // was bluetooth true/false before
} SettingsV10;

// When incrementing SETTINGS_VERSION, update the Settings typedef and add
// backwards compatibility to InitSettings!
#define SETTINGS_VERSION 10
typedef SettingsV10 Settings;
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
	.toggled_brightness = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_colortemperature = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_contrast = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_saturation = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_exposure = SETTINGS_DEFAULT_MUTE_NO_CHANGE,
	.toggled_volume = 0, // mute is default
	.disable_dpad_on_mute = 0,
	.emulate_joystick_on_mute = 0,
	.turbo_a = 0,
	.turbo_b = 0,
	.turbo_x = 0,
	.turbo_y = 0,
	.turbo_l1 = 0,
	.turbo_l2 = 0,
	.turbo_r1 = 0,
	.turbo_r2 = 0,
	.jack = 0,
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

static int is_brick = 0;

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
					if(version==9) {
						printf("Found settings v9.\n");
						SettingsV9 old;
						read(fd, &old, sizeof(SettingsV9));

						settings-> disable_dpad_on_mute = old.disable_dpad_on_mute;
						settings-> emulate_joystick_on_mute = old.emulate_joystick_on_mute;
						settings-> turbo_a = old.turbo_a;
						settings-> turbo_b = old.turbo_b;
						settings-> turbo_x = old.turbo_x;
						settings-> turbo_y = old.turbo_y;
						settings-> turbo_l1 = old.turbo_l1;
						settings-> turbo_l2 = old.turbo_l2;
						settings-> turbo_r1 = old.turbo_r1;
						settings-> turbo_r2 = old.turbo_r2;

						settings->toggled_volume = old.toggled_volume;

						settings->toggled_brightness = old.toggled_brightness;
						settings->toggled_colortemperature = old.toggled_colortemperature;
						settings->toggled_contrast = old.toggled_contrast;
						settings->toggled_exposure = old.toggled_exposure;
						settings->toggled_saturation = old.toggled_saturation;

						settings->saturation = old.saturation;
						settings->contrast = old.contrast;
						settings->exposure = old.exposure;

						settings->colortemperature = old.colortemperature;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==8) {
						printf("Found settings v8.\n");
						SettingsV8 old;
						read(fd, &old, sizeof(SettingsV8));

						settings->toggled_volume = old.toggled_volume;

						settings->toggled_brightness = old.toggled_brightness;
						settings->toggled_colortemperature = old.toggled_colortemperature;
						settings->toggled_contrast = old.toggled_contrast;
						settings->toggled_exposure = old.toggled_exposure;
						settings->toggled_saturation = old.toggled_saturation;

						settings->saturation = old.saturation;
						settings->contrast = old.contrast;
						settings->exposure = old.exposure;

						settings->colortemperature = old.colortemperature;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==7) {
						printf("Found settings v7.\n");
						SettingsV7 old;
						read(fd, &old, sizeof(SettingsV7));

						// muted* -> toggled*
						settings->toggled_brightness = old.mutedbrightness;
						settings->toggled_colortemperature = old.mutedcolortemperature;
						settings->toggled_contrast = old.mutedcontrast;
						settings->toggled_exposure = old.mutedexposure;
						settings->toggled_saturation = old.mutedsaturation;

						settings->saturation = old.saturation;
						settings->contrast = old.contrast;
						settings->exposure = old.exposure;

						settings->colortemperature = old.colortemperature;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==6) {
						printf("Found settings v6.\n");
						SettingsV6 old;
						read(fd, &old, sizeof(SettingsV6));

						settings->saturation = old.saturation;
						settings->contrast = old.contrast;
						settings->exposure = old.exposure;

						settings->colortemperature = old.colortemperature;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==5) {
						printf("Found settings v5.\n");
						SettingsV5 old;
						read(fd, &old, sizeof(SettingsV5));

						settings->colortemperature = old.colortemperature;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==4) {
						printf("Found settings v4.\n");
						SettingsV4 old;
						read(fd, &old, sizeof(SettingsV4));

						// colortemp was 0-20 here
						settings->colortemperature = old.colortemperature * 2;

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
					}
					else if(version==3) {
						printf("Found settings v3.\n");
						SettingsV3 old;
						read(fd, &old, sizeof(SettingsV3));

						settings->brightness = old.brightness;
						settings->headphones = old.headphones;
						settings->speaker = old.speaker;
						settings->mute = old.mute;
						settings->jack = old.jack;
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

	// int jack = JACK_enabled();
	// int hdmi = HDMI_enabled();
	// printf("brightness: %i (hdmi: %i)\nspeaker: %i (jack: %i)\n", settings->brightness, hdmi, settings->speaker, jack); fflush(stdout);

	// // both of these set volume
	// SetJack(jack);
	// SetHDMI(hdmi);

	// char cmd[256];
	// sprintf(cmd, "amixer sset 'Playback Path' '%s' > /dev/null 2>&1", GetJack() ? "HP" : "SPK");
	// system(cmd);

	// SetVolume(GetVolume());
	// SetBrightness(GetBrightness());
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

int GetBrightness(void) { // 0-10
	return settings->brightness;
}
int GetColortemp(void) { // 0-10
	return settings->colortemperature;
}
int GetVolume(void) { // 0-20
	if (settings->mute && GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
		return GetMutedVolume();

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
	return 0;
};

int GetMute(void) {
	return settings->mute;
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
	return settings->toggled_brightness;
}
int GetMutedColortemp(void)
{
	return settings->toggled_colortemperature;
}
int GetMutedContrast(void)
{
	return settings->toggled_contrast;
}
int GetMutedSaturation(void)
{
	return settings->toggled_saturation;
}
int GetMutedExposure(void)
{
	return settings->toggled_exposure;
}
int GetMutedVolume(void)
{
	return settings->toggled_volume;
}
int GetMuteDisablesDpad(void)
{
	return settings->disable_dpad_on_mute;
}
int GetMuteEmulatesJoystick(void)
{
	return settings->emulate_joystick_on_mute;
}
int GetMuteTurboA(void)
{
	return settings->turbo_a;
}
int GetMuteTurboB(void)
{
	return settings->turbo_b;
}
int GetMuteTurboX(void)
{
	return settings->turbo_x;
}
int GetMuteTurboY(void)
{
	return settings->turbo_y;
}
int GetMuteTurboL1(void)
{
	return settings->turbo_l1;
}
int GetMuteTurboL2(void)
{
	return settings->turbo_l2;
}
int GetMuteTurboR1(void)
{
	return settings->turbo_r1;
}
int GetMuteTurboR2(void)
{
	return settings->turbo_r2;
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
	if (settings->mute)
		return SetRawVolume(scaleVolume(GetMutedVolume()));

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

void SetHDMI(int value){};

void SetMute(int value) {
	settings->mute = value;
	if (settings->mute) {
		if (GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawVolume(scaleVolume(GetMutedVolume()));
		// custom mute mode display settings
		if(GetMutedBrightness() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawBrightness(scaleBrightness(GetMutedBrightness()));
		if(GetMutedColortemp() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawColortemp(scaleColortemp(GetMutedColortemp()));
		if(GetMutedContrast() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawContrast(scaleContrast(GetMutedContrast()));
		if(GetMutedSaturation() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawSaturation(scaleSaturation(GetMutedSaturation()));
		if(GetMutedExposure() != SETTINGS_DEFAULT_MUTE_NO_CHANGE)
			SetRawExposure(scaleExposure(GetMutedExposure()));
		if(is_brick && GetMuteDisablesDpad())
			disableDpad(1);
		if(is_brick && GetMuteEmulatesJoystick())
			emulateJoystick(1);
		if(GetMuteTurboA())
			turboA(1);
		if(GetMuteTurboB())
			turboB(1);
		if(GetMuteTurboX())
			turboX(1);
		if(GetMuteTurboY())
			turboY(1);
		if(GetMuteTurboL1())
			turboL1(1);
		if(GetMuteTurboL2())
			turboL2(1);
		if(GetMuteTurboR1())
			turboR1(1);
		if(GetMuteTurboR2())
			turboR2(1);
	}
	else {
		SetVolume(GetVolume());
		SetBrightness(GetBrightness());
		SetColortemp(GetColortemp());
		SetContrast(GetContrast());
		SetSaturation(GetSaturation());
		SetExposure(GetExposure());
		if(is_brick) {
			if(GetMuteDisablesDpad())
				disableDpad(0);
			if(GetMuteEmulatesJoystick())
				emulateJoystick(0);
		}
		if(GetMuteTurboA())
			turboA(0);
		if(GetMuteTurboB())
			turboB(0);
		if(GetMuteTurboX())
			turboX(0);
		if(GetMuteTurboY())
			turboY(0);
		if(GetMuteTurboL1())
			turboL1(0);
		if(GetMuteTurboL2())
			turboL2(0);
		if(GetMuteTurboR1())
			turboR1(0);
		if(GetMuteTurboR2())
			turboR2(0);
	}
}
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
	settings->toggled_brightness = value;
	SaveSettings();
}

void SetMutedColortemp(int value)
{
	settings->toggled_colortemperature = value;
	SaveSettings();
}

void SetMutedContrast(int value)
{
	settings->toggled_contrast = value;
	SaveSettings();
}

void SetMutedSaturation(int value)
{
	settings->toggled_saturation = value;
	SaveSettings();
}

void SetMutedExposure(int value)
{
	settings->toggled_exposure = value;
	SaveSettings();
}

void SetMutedVolume(int value)
{
	settings->toggled_volume = value;
	SaveSettings();
}

void SetMuteDisablesDpad(int value)
{
	settings->disable_dpad_on_mute = value;
	SaveSettings();
}
void SetMuteEmulatesJoystick(int value)
{
	settings->emulate_joystick_on_mute = value;
	SaveSettings();
}

void SetMuteTurboA(int value)
{
	settings->turbo_a = value;
	SaveSettings();
}

void SetMuteTurboB(int value)
{
	settings->turbo_b = value;
	SaveSettings();
}

void SetMuteTurboX(int value)
{
	settings->turbo_x = value;
	SaveSettings();
}

void SetMuteTurboY(int value)
{
	settings->turbo_y = value;
	SaveSettings();
}

void SetMuteTurboL1(int value)
{
	settings->turbo_l1 = value;
	SaveSettings();
}

void SetMuteTurboL2(int value)
{
	settings->turbo_l2 = value;
	SaveSettings();
}

void SetMuteTurboR1(int value)
{
	settings->turbo_r1 = value;
	SaveSettings();
}

void SetMuteTurboR2(int value)
{
	settings->turbo_r2 = value;
	SaveSettings();
}

///////// trimui_inputd modifiers

#define INPUTD_PATH "/tmp/trimui_inputd"
#define INPUTD_DPAD_PATH "/tmp/trimui_inputd/input_no_dpad"
#define INPUTD_JOYSTICK_PATH "/tmp/trimui_inputd/input_dpad_to_joystick"
#define INPUTD_TURBO_A_PATH "/tmp/trimui_inputd/turbo_a"
#define INPUTD_TURBO_B_PATH "/tmp/trimui_inputd/turbo_b"
#define INPUTD_TURBO_X_PATH "/tmp/trimui_inputd/turbo_x"
#define INPUTD_TURBO_Y_PATH "/tmp/trimui_inputd/turbo_y"
#define INPUTD_TURBO_L1_PATH "/tmp/trimui_inputd/turbo_l"
#define INPUTD_TURBO_L2_PATH "/tmp/trimui_inputd/turbo_l2"
#define INPUTD_TURBO_R1_PATH "/tmp/trimui_inputd/turbo_r"
#define INPUTD_TURBO_R2_PATH "/tmp/trimui_inputd/turbo_r2"

void disableDpad(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_DPAD_PATH);
	}
	else {
		unlink(INPUTD_DPAD_PATH);
	}
}

void emulateJoystick(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_JOYSTICK_PATH);
	}
	else {
		unlink(INPUTD_JOYSTICK_PATH);
	}
}

void turboA(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_A_PATH);
	}
	else {
		unlink(INPUTD_TURBO_A_PATH);
	}
}
void turboB(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_B_PATH);
	}
	else {
		unlink(INPUTD_TURBO_B_PATH);
	}
}
void turboX(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_X_PATH);
	}
	else {
		unlink(INPUTD_TURBO_X_PATH);
	}
}
void turboY(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_Y_PATH);
	}
	else {
		unlink(INPUTD_TURBO_Y_PATH);
	}
}
void turboL1(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_L1_PATH);
	}
	else {
		unlink(INPUTD_TURBO_L1_PATH);
	}
}
void turboL2(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_L2_PATH);
	}
	else {
		unlink(INPUTD_TURBO_L2_PATH);
	}
}
void turboR1(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_R1_PATH);
	}
	else {
		unlink(INPUTD_TURBO_R1_PATH);
	}
}
void turboR2(int value) {
	if(value) {
		mkdir(INPUTD_PATH, 0755);
		touch(INPUTD_TURBO_R2_PATH);
	}
	else {
		unlink(INPUTD_TURBO_R2_PATH);
	}
}

///////// Platform specific scaling

int scaleVolume(int value) {
	return value * 5; // scale 0-20 to 0-100
}

int scaleBrightness(int value) {
	int raw;
	if (is_brick) {
		switch (value) {
			case 0: raw=1; break; 		// 0
			case 1: raw=8; break; 		// 8
			case 2: raw=16; break; 		// 8
			case 3: raw=32; break; 		// 16
			case 4: raw=48; break;		// 16
			case 5: raw=72; break;		// 24
			case 6: raw=96; break;		// 24
			case 7: raw=128; break;		// 32
			case 8: raw=160; break;		// 32
			case 9: raw=192; break;		// 32
			case 10: raw=255; break;	// 64
		}
	}
	else {
		switch (value) {
			case 0: raw=4; break; 		//  0
			case 1: raw=6; break; 		//  2
			case 2: raw=10; break; 		//  4
			case 3: raw=16; break; 		//  6
			case 4: raw=32; break;		// 16
			case 5: raw=48; break;		// 16
			case 6: raw=64; break;		// 16
			case 7: raw=96; break;		// 32
			case 8: raw=128; break;		// 32
			case 9: raw=192; break;		// 64
			case 10: raw=255; break;	// 64
		}
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

#define DISP_LCD_SET_BRIGHTNESS  0x102
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
void SetRawVolume(int val) { // 0-100

	char cmd[256];
	sprintf(cmd, "amixer sset 'SPK' -M %i%% &> /dev/null", val);
	system(cmd);


	// TODO: unfortunately doing it this way creating a linker nightmare
	// struct mixer *mixer = mixer_open(0);
	// struct mixer_ctl *ctl;
	//
	// // digital volume (one-time?)
	// ctl = mixer_get_ctl(mixer, 3);
	// mixer_ctl_set_value(ctl,0,0);
	//
	// // Soft Volume Master (one-time?)
	// ctl = mixer_get_ctl(mixer, 16);
	// mixer_ctl_set_value(ctl,0,255);
	// mixer_ctl_set_value(ctl,1,255);
	//
	// // DAC volume
	// ctl = mixer_get_ctl(mixer, 7);
	// mixer_ctl_set_value(ctl,0,val);
	// mixer_ctl_set_value(ctl,1,val);
	// mixer_close(mixer);

	// char cmd[256];
	// sprintf(cmd, "amixer sset 'digital volume' %i%% &> /dev/null", 100-val);
	// // puts(cmd); fflush(stdout);
	// system(cmd);
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

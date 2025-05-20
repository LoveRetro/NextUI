#include "msettings.h"

// desktop
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
//#include <sys/mman.h>
//#include <sys/ioctl.h>
//#include <errno.h>
//#include <sys/stat.h>
//#include <dlfcn.h>
#include <string.h>

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

// Current NextUI settings format
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
} SettingsV8;

// When incrementing SETTINGS_VERSION, update the Settings typedef and add
// backwards compatibility to InitSettings!
#define SETTINGS_VERSION 8
typedef SettingsV8 Settings;
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
	.jack = 0,
};
static Settings* msettings;

static char SettingsPath[256];

///////////////////////////////////////

int peekVersion(const char *filename) {
	int version = 0;
	FILE *file = fopen(filename, "r");
	if (file) {
		fread(&version, sizeof(int), 1, file);
		fclose(file);
	}
	return version;
}

void InitSettings(void){
	// We are not really using them, but we should be able to debug them
	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));
	//sprintf(SettingsPath, "%s/msettings.bin", SDCARD_PATH "/.userdata");
	msettings = (Settings*)malloc(sizeof(Settings));
	
	int version = peekVersion(SettingsPath);
	if(version > 0) {
		// fopen file pointer
		int fd = open(SettingsPath, O_RDONLY);
		if(fd) {
			if (version == SETTINGS_VERSION) {
				read(fd, msettings, sizeof(Settings));
			}
			else if(version==7) {
				SettingsV7 old;
				read(fd, &old, sizeof(SettingsV7));
				// default muted
				msettings->toggled_volume = 0;
				// muted* -> toggled*
				msettings->toggled_brightness = old.mutedbrightness;
				msettings->toggled_colortemperature = old.mutedcolortemperature;
				msettings->toggled_contrast = old.mutedcontrast;
				msettings->toggled_exposure = old.mutedexposure;
				msettings->toggled_saturation = old.mutedsaturation;
				// copy the rest
				msettings->saturation = old.saturation;
				msettings->contrast = old.contrast;
				msettings->exposure = old.exposure;
				msettings->colortemperature = old.colortemperature;
				msettings->brightness = old.brightness;
				msettings->headphones = old.headphones;
				msettings->speaker = old.speaker;
				msettings->mute = old.mute;
				msettings->jack = old.jack;
			}
			else if(version==6) {
				SettingsV6 old;
				read(fd, &old, sizeof(SettingsV6));
				// no muted* settings yet, default values used.
				msettings->toggled_brightness = SETTINGS_DEFAULT_MUTE_NO_CHANGE;
				msettings->toggled_colortemperature = SETTINGS_DEFAULT_MUTE_NO_CHANGE;
				msettings->toggled_contrast = SETTINGS_DEFAULT_MUTE_NO_CHANGE;
				msettings->toggled_exposure = SETTINGS_DEFAULT_MUTE_NO_CHANGE;
				msettings->toggled_saturation = SETTINGS_DEFAULT_MUTE_NO_CHANGE;
				// copy the rest
				msettings->saturation = old.saturation;
				msettings->contrast = old.contrast;
				msettings->exposure = old.exposure;
				msettings->colortemperature = old.colortemperature;
				msettings->brightness = old.brightness;
				msettings->headphones = old.headphones;
				msettings->speaker = old.speaker;
				msettings->mute = old.mute;
				msettings->jack = old.jack;
			}
			else if(version==5) {
				SettingsV5 old;
				read(fd, &old, sizeof(SettingsV5));
				// no display settings yet, default values used. 
				msettings->saturation = 0;
				msettings->contrast = 0;
				msettings->exposure = 0;
				// copy the rest
				msettings->colortemperature = old.colortemperature;
				msettings->brightness = old.brightness;
				msettings->headphones = old.headphones;
				msettings->speaker = old.speaker;
				msettings->mute = old.mute;
				msettings->jack = old.jack;
			}
			else if(version==4) {
				printf("Found settings v4.\n");
				SettingsV4 old;
				// read old settings from fd
				read(fd, &old, sizeof(SettingsV4));
				// colortemp was 0-20 here
				msettings->colortemperature = old.colortemperature * 2;
			}
			else if(version==3) {
				printf("Found settings v3.\n");
				SettingsV3 old;
				read(fd, &old, sizeof(SettingsV3));
				// no colortemp setting yet, default value used. 
				// copy the rest
				msettings->brightness = old.brightness;
				msettings->headphones = old.headphones;
				msettings->speaker = old.speaker;
				msettings->mute = old.mute;
				msettings->jack = old.jack;
				msettings->colortemperature = 20;
			}
			else {
				printf("Found unsupported settings version: %i.\n", version);
				// load defaults
				memcpy(msettings, &DefaultSettings, sizeof(Settings));
			}

			close(fd);
		}
		else {
			printf("Unable to read settings, using defaults\n");
			// load defaults
			memcpy(msettings, &DefaultSettings, sizeof(Settings));
		}
	}
	else {
		printf("No settings found, using defaults\n");
		// load defaults
		memcpy(msettings, &DefaultSettings, sizeof(Settings));
	}
}
void QuitSettings(void){
	// dealloc settings
	free(msettings);
	msettings = NULL;
}
int InitializedSettings(void)
{
	return msettings != NULL;
}

// not implemented here

int GetBrightness(void) { return 0; }
int GetColortemp(void) { return 0; }
int GetContrast(void) { return 0; }
int GetSaturation(void) { return 0; }
int GetExposure(void) { return 0; }
int GetVolume(void) { return 0; }

int GetMutedBrightness(void) { return 0; }
int GetMutedColortemp(void) { return 0; }
int GetMutedContrast(void) { return 0; }
int GetMutedSaturation(void) { return 0; }
int GetMutedExposure(void) { return 0; }
int GetMutedVolume(void) { return 0; }

void SetMutedBrightness(int value){}
void SetMutedColortemp(int value){}
void SetMutedContrast(int value){}
void SetMutedSaturation(int value){}
void SetMutedExposure(int value){}
void SetMutedVolume(int value){}

void SetRawBrightness(int value) {}
void SetRawVolume(int value){}

void SetBrightness(int value) {}
void SetColortemp(int value) {}
void SetContrast(int value) {}
void SetSaturation(int value) {}
void SetExposure(int value) {}
void SetVolume(int value) {}

int GetJack(void) { return 0; }
void SetJack(int value) {}

int GetHDMI(void) { return 0; }
void SetHDMI(int value) {}

int GetMute(void) { return 0; }
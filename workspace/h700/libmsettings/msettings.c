// h700
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

#include "displaycal.h"
#include "msettings.h"
#include "sunxi_display2_min.h"

///////////////////////////////////////

// First H700 settings format. Prerelease formats intentionally reset to defaults.
typedef struct SettingsV1 {
	int version; // future proofing
	int brightness;
	int colortemperature;
	int headphones;
	int speaker;
	int contrast;
	int saturation;
	int exposure;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack;
	int audiosink; // AUDIO_SINK_*
	int displaycal_enabled;
	int displaycal_red_gain;
	int displaycal_green_gain;
	int displaycal_blue_gain;
} SettingsV1;

// Future format changes must update this version and the loader together.
#define SETTINGS_VERSION 1
typedef SettingsV1 Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = SETTINGS_DEFAULT_BRIGHTNESS,
	.colortemperature = SETTINGS_DEFAULT_COLORTEMP,
	.headphones = SETTINGS_DEFAULT_HEADPHONE_VOLUME,
	.speaker = SETTINGS_DEFAULT_VOLUME,
	.contrast = SETTINGS_DEFAULT_CONTRAST,
	.saturation = SETTINGS_DEFAULT_SATURATION,
	.exposure = SETTINGS_DEFAULT_EXPOSURE,
	.jack = 0,
	.audiosink = AUDIO_SINK_DEFAULT,
	.displaycal_enabled = DISPLAYCAL_DEFAULT_ENABLED,
	.displaycal_red_gain = DISPLAYCAL_DEFAULT_RED_GAIN,
	.displaycal_green_gain = DISPLAYCAL_DEFAULT_GREEN_GAIN,
	.displaycal_blue_gain = DISPLAYCAL_DEFAULT_BLUE_GAIN,
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

static int prefixMatch(char* pre, char* str) {
	if (!pre || !str) return 0;
	return (strncmp(pre, str, strlen(pre))==0);
}

// Displaycal defaults for this DEVICE. Measured panels keep their gains;
// everything else stays uncalibrated (off + 100/100/100).
static enum DisplayCalPreset displayCalPresetForDevice(void) {
	char* device = getenv("DEVICE");

	if (exactMatch("rg28xx", device)) return DISPLAYCAL_PRESET_RG28XX;
	if (exactMatch("rgsp", device)) return DISPLAYCAL_PRESET_RGSP;
	if (exactMatch("rg34xxsp", device)) return DISPLAYCAL_PRESET_RG34XXSP;
	if (exactMatch("rg34xx", device)) return DISPLAYCAL_PRESET_RG34XX;
	if (exactMatch("rg40xxv", device)) return DISPLAYCAL_PRESET_RG40XXV;
	if (exactMatch("rg40xxh", device)) return DISPLAYCAL_PRESET_RG40XXH;
	if (exactMatch("rgcubexx", device)) return DISPLAYCAL_PRESET_RGCUBEXX;
	if (exactMatch("rg35xxsp", device)) return DISPLAYCAL_PRESET_RG35XXSP;
	if (exactMatch("rg35xxpro", device)) return DISPLAYCAL_PRESET_RG35XXPRO;
	if (exactMatch("rg35xxh", device)) return DISPLAYCAL_PRESET_RG35XXH;
	if (exactMatch("rg35xxplus", device)) return DISPLAYCAL_PRESET_RG35XXPLUS;
	return DISPLAYCAL_PRESET_DEFAULT;
}

static void applyDisplayCalDefaultsForDevice(Settings *target) {
	DisplayCalDefaults defaults = DisplayCal_getDefaultSettings(displayCalPresetForDevice());
	target->displaycal_enabled = defaults.enabled;
	target->displaycal_red_gain = defaults.red_gain;
	target->displaycal_green_gain = defaults.green_gain;
	target->displaycal_blue_gain = defaults.blue_gain;
}

// Accept only a complete V1 record; old prerelease and damaged files use defaults.
static void loadSettings(void) {
	FILE *file = fopen(SettingsPath, "rb");
	if (!file) return;

	Settings saved;
	if (fread(&saved, sizeof(saved), 1, file) == 1 &&
		saved.version == SETTINGS_VERSION && fgetc(file) == EOF && !ferror(file)) {
		memcpy(settings, &saved, sizeof(saved));
	}
	fclose(file);
}

void InitSettings(void) {
	applyDisplayCalDefaultsForDevice(&DefaultSettings);

	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));
	
	shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		// puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	else { // host
		// puts("Settings host"); // keymon
		is_host = 1;
		// we created it so set initial size and populate
		ftruncate(shm_fd, shm_size);
		settings = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

		memcpy(settings, &DefaultSettings, shm_size);
		loadSettings();
	}
	// printf("brightness: %i\nspeaker: %i \n", settings->brightness, settings->speaker);
	// Log mixer state before changing routing to help diagnose Bluetooth/USB DAC issues.
	system("amixer");

	// Make sure the H700 codec is routed to speaker/lineout before NextUI owns volume.
	if(GetAudioSink() == AUDIO_SINK_DEFAULT) {
		system("amixer -q sset 'SPK' on >/dev/null 2>&1");
		system("amixer -q sset 'LINEOUT' on >/dev/null 2>&1");
		system("amixer -q sset 'OutputL Mixer DACL' on >/dev/null 2>&1");
		system("amixer -q sset 'OutputR Mixer DACR' on >/dev/null 2>&1");
	}

	// Apply the current hardware settings without TrimUI FN-switch overrides.
	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
	SetColortemp(GetColortemp());
	SetContrast(GetContrast());
	SetSaturation(GetSaturation());
	SetExposure(GetExposure());
	if (settings->displaycal_enabled)
		SetRawDisplayCal(1, settings->displaycal_red_gain, settings->displaycal_green_gain, settings->displaycal_blue_gain);
}
int InitializedSettings(void) {
	return (settings != NULL);
}
void QuitSettings(void) {
	munmap(settings, shm_size);
	if (is_host) shm_unlink(SHM_KEY);
}
static inline void SaveSettings(void) {
	int fd = open(SettingsPath, O_CREAT|O_WRONLY|O_TRUNC, 0644);
	if (fd>=0) {
		write(fd, settings, shm_size);
		close(fd);
		sync();
	}
}

static inline void applyDisplayCalSettings(void) {
	if (settings->displaycal_enabled)
		SetRawDisplayCal(1, settings->displaycal_red_gain, settings->displaycal_green_gain, settings->displaycal_blue_gain);
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
	return settings->displaycal_enabled;
}
int GetDisplayCalRedGain(void)
{
	return settings->displaycal_red_gain;
}
int GetDisplayCalGreenGain(void)
{
	return settings->displaycal_green_gain;
}
int GetDisplayCalBlueGain(void)
{
	return settings->displaycal_blue_gain;
}
// TODO: wire up headphone-jack detection in H700 keymon.
int GetJack(void) {
	return settings->jack;
}
// monitored and set by thread in audiomon
int GetAudioSink(void) {
	return settings->audiosink;
}

int GetHDMI(void) { 
	if (getInt("/sys/class/extcon/hdmi/state") > 0)
		return 1;
	if (getInt("/sys/class/extcon/hdmi/cable.0/state") > 0)
		return 1;
	if (getInt("/sys/class/extcon/hdmi/cable.1/state") > 0)
		return 1;
	return 0;
};

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
void SetContrast(int value) {
	SetRawContrast(scaleContrast(value));
	settings->contrast = value;
	SaveSettings();
}
void SetSaturation(int value) {
	SetRawSaturation(scaleSaturation(value));
	settings->saturation = value;
	SaveSettings();
}
void SetExposure(int value){
	SetRawExposure(scaleExposure(value));
	settings->exposure = value;
	SaveSettings();
}
void SetDisplayCalEnabled(int is_enabled) {
	int was_enabled = settings->displaycal_enabled;
	is_enabled = (is_enabled != 0);
	settings->displaycal_enabled = is_enabled;

	// Disabling only needs hardware writes when we are turning an active LUT off.
	if (is_enabled)
		applyDisplayCalSettings();
	else if (was_enabled)
		SetRawDisplayCal(0, settings->displaycal_red_gain, settings->displaycal_green_gain, settings->displaycal_blue_gain);
	SaveSettings();
}
void SetDisplayCalRedGain(int value) {
	value = DisplayCal_clampGainValue(value);
	settings->displaycal_red_gain = value;
	applyDisplayCalSettings();
	SaveSettings();
}
void SetDisplayCalGreenGain(int value) {
	value = DisplayCal_clampGainValue(value);
	settings->displaycal_green_gain = value;
	applyDisplayCalSettings();
	SaveSettings();
}
void SetDisplayCalBlueGain(int value) {
	value = DisplayCal_clampGainValue(value);
	settings->displaycal_blue_gain = value;
	applyDisplayCalSettings();
	SaveSettings();
}
void SetVolume(int value) // 0-20
{
	SetRawVolume(scaleVolume(value));
	if (settings->jack || settings->audiosink != AUDIO_SINK_DEFAULT)
		settings->headphones = value;
	else
		settings->speaker = value;
	SaveSettings();
}
// TODO: wire up headphone-jack detection in H700 keymon.
void SetJack(int value) {
	settings->jack = value;
	SetVolume(GetVolume());
}
// monitored and set by thread in audiomon
void SetAudioSink(int value) {
	settings->audiosink = value;
	SetVolume(GetVolume());
}

// HDMI output switching (sunxi disp2, kernel 4.9 BSP). The dispdbg debugfs
// interface flips disp0 between the LCD and the HDMI TX; the framebuffer is
// then resized so the mali EGL winsys (which latches fb0 geometry at video
// init) comes up at the matching resolution on the next app start. Sequence
// and parameters replicate the old rg35xxplus port's hdmimon.sh (proven on
// this hardware family): HDMI is a 1080p60 signal fed by a 1280x720 fb that
// the display engine hardware-scales up. Callers are expected to restart the
// UI afterwards; the switch itself must run while no EGL surface exists.

#define DISPDBG_PATH "/sys/kernel/debug/dispdbg"
#define FB_BLANK_PATH "/sys/class/graphics/fb0/blank"
// must match HDMI_WIDTH/HDMI_HEIGHT in platform.h
#define HDMI_LOGICAL_WIDTH 1280
#define HDMI_LOGICAL_HEIGHT 720
// dispdbg switch params: "<output type> <mode> ..." — type 4=HDMI, mode 10=1080p60; type 1=LCD
#define DISP_SWITCH_HDMI_PARAM "4 10 0 0 0x4 0x101 0 0 0 8"
#define DISP_SWITCH_LCD_PARAM "1 0"
#define ASOUNDRC_HDMI_MARKER "# nextui-hdmi"

static void panelSize(int* w, int* h) {
	char* device = getenv("DEVICE");
	*w = 640; *h = 480;
	if (exactMatch("rgcubexx", device)) { *w = 720; *h = 720; }
	else if (exactMatch("rg34xx", device) || exactMatch("rg34xxsp", device)
		|| exactMatch("rgsp", device)) { *w = 720; *h = 480; }
	else if (exactMatch("rg28xx", device)) { *w = 480; *h = 640; } // physical portrait panel
}

// the live output type, read back from the disp driver so a stale state file
// can never desync us from the hardware
static int hdmiOutputActive(void) {
	char buffer[4096] = {0};
	FILE* file = fopen("/sys/class/disp/disp/attr/sys", "r");
	if (!file) return 0;
	size_t len = fread(buffer, 1, sizeof(buffer)-1, file);
	fclose(file);
	buffer[len] = '\0';
	return strstr(buffer, "hdmi output") != NULL;
}

// The dispdbg switch is asynchronous and slow — the LCD panel init sequence
// alone takes ~700ms ("attached ok" in dmesg). The fb geometry set below is
// what rebuilds the DE layer against the new output, so it must not run until
// the attach has completed or the rebuild is computed against the old output
// and the panel shows a stale (blank) layer.
static void waitForOutput(int hdmi) {
	for (int i = 0; i < 60; i++) { // up to 3s
		if (hdmiOutputActive() == hdmi) break;
		usleep(50000);
	}
	usleep(150000); // settle margin after the attach shows up in sysfs
}

static void dispdbgSwitch(char* param) {
	putFile(DISPDBG_PATH "/name", "disp0");
	putFile(DISPDBG_PATH "/command", "switch");
	putFile(DISPDBG_PATH "/param", param);
	putFile(DISPDBG_PATH "/start", "1");
}

static void clearFramebuffer(void) {
	int fd = open("/dev/fb0", O_WRONLY);
	if (fd < 0) return;
	char zeros[4096] = {0};
	while (write(fd, zeros, sizeof(zeros)) > 0);
	close(fd);
}

static void setFramebufferSize(int w, int h) {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) return;
	struct fb_var_screeninfo vinfo;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
		// zero geometry first ("fbset -g 0 0 0 0 32" in the old port) so the
		// driver drops the old allocation before sizing the new one
		vinfo.xres = vinfo.yres = vinfo.xres_virtual = vinfo.yres_virtual = 0;
		vinfo.bits_per_pixel = 32;
		ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo); // failure is fine, best effort
		usleep(250000);
		vinfo.xres = w;
		vinfo.yres = h;
		vinfo.xres_virtual = w;
		vinfo.yres_virtual = h * 2; // double buffered
		// The pan offset survives the resize: coming off the 640x480 panel it is
		// still 480, which is not a page boundary of the new geometry. Left
		// there it wedges the winsys — measured on RG40XXV, fb0 then never pans
		// again and every frame lands in page 0. Reset it so the buffer starts
		// on page 0 and the blob's flips work from a sane origin.
		vinfo.xoffset = vinfo.yoffset = 0;
		vinfo.bits_per_pixel = 32;
		// FORCE so set_par runs (and rebuilds the DE layer for the new output)
		// even when the geometry is unchanged
		vinfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo) < 0)
			fprintf(stderr, "SetHDMI: FBIOPUT_VSCREENINFO %dx%d failed: %s\n", w, h, strerror(errno));
	}
	close(fd);
}

// The fbdev glue in this BSP has no set_par hook: FBIOPUT only records the new
// var, and pan_display only rewrites the layer *crop* from it — the scanout
// layer's buffer dimensions and output window are never updated by any fbdev
// call. Left alone, whichever values the switch path last committed stay live
// (blank panel after unplug, page-offset ghosting after plug). So after every
// switch we read-modify-write the fb0 layer ourselves with the geometry that
// matches the new framebuffer and output; the blob's per-flip pans then keep
// the crop in sync against our committed config.
// The crop we commit must name page 0, matching the pan offset setFramebufferSize
// just reset. Naming page 1 was only ever right if a flip landed before the first
// scanout; when none does — which is what happens if the winsys is wedged, and
// what happened on BaseOS — the display shows a page nothing has written, i.e.
// black on both the TV and the panel.
// The switch path restores the previous mode's layer config on device attach,
// asynchronously and sometimes AFTER we've already written ours — whichever
// commit lands last wins. So: commit, read back, and retry until it sticks.
// (The app hasn't started yet when this runs, so there are no competing pans.)
static void commitLayerGeometry(int fbW, int fbH, int outW, int outH) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) return;
	struct disp_layer_config config;
	unsigned long param[4] = {0, (unsigned long)&config, 1, 0};
	int applied = 0;
	for (int attempt = 0; attempt < 20 && !applied; attempt++) { // up to ~2s
		memset(&config, 0, sizeof(config));
		config.channel = 1; // fb0 is bound to channel 1, layer 0 (FBIO_ALLOC in dev_fb.c)
		config.layer_id = 0;
		if (ioctl(fd, DISP_LAYER_GET_CONFIG, param) < 0) {
			fprintf(stderr, "SetHDMI: DISP_LAYER_GET_CONFIG failed: %s\n", strerror(errno));
			break;
		}
		config.enable = 1;
		for (int i = 0; i < 3; i++) {
			config.info.fb.size[i].width = fbW;
			config.info.fb.size[i].height = fbH * 2; // whole double buffer
		}
		config.info.fb.crop.x = 0;
		config.info.fb.crop.y = 0; // page 0 — see above
		config.info.fb.crop.width = ((long long)fbW) << 32;
		config.info.fb.crop.height = ((long long)fbH) << 32;
		config.info.screen_win.x = 0;
		config.info.screen_win.y = 0;
		config.info.screen_win.width = outW;
		config.info.screen_win.height = outH;
		if (ioctl(fd, DISP_LAYER_SET_CONFIG, param) < 0) {
			fprintf(stderr, "SetHDMI: DISP_LAYER_SET_CONFIG failed: %s\n", strerror(errno));
			break;
		}
		usleep(100000); // let the vsync apply run (and any late restore overwrite us)
		memset(&config, 0, sizeof(config));
		config.channel = 1;
		config.layer_id = 0;
		if (ioctl(fd, DISP_LAYER_GET_CONFIG, param) < 0)
			break;
		applied = config.info.fb.size[0].width == (unsigned)fbW
			&& config.info.screen_win.width == (unsigned)outW
			&& config.info.fb.crop.y == 0; // a late restore can revive the old page offset
	}
	if (!applied)
		fprintf(stderr, "SetHDMI: layer geometry %dx%d->%dx%d did not stick\n", fbW, fbH, outW, outH);
	close(fd);
}

// route ALSA "default" to the HDMI TX while connected; only ever touch an
// .asoundrc we wrote ourselves so audiomon's Bluetooth/USB routing wins
static void setHDMIAudioRoute(int on) {
	char* home = getenv("USERDATA_PATH");
	if (!home) home = getenv("HOME");
	if (!home) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/.asoundrc", home);

	char buffer[64] = {0};
	FILE* file = fopen(path, "r");
	if (file) {
		fread(buffer, 1, sizeof(buffer)-1, file);
		fclose(file);
		if (!prefixMatch(ASOUNDRC_HDMI_MARKER, buffer)) return; // not ours
	}

	if (on) {
		putFile(path,
			ASOUNDRC_HDMI_MARKER "\n"
			"pcm.!default {\n"
			"    type plug\n"
			"    slave.pcm {\n"
			"        type hw\n"
			"        card ahubhdmi\n"
			"    }\n"
			"}\n"
			"ctl.!default {\n"
			"    type hw\n"
			"    card ahubhdmi\n"
			"}\n");
	}
	else if (file) unlink(path); // only removes the file when it carried our marker
}

void SetHDMI(int value) {
	value = value ? 1 : 0;
	if (hdmiOutputActive() == value) {
		setHDMIAudioRoute(value); // keep audio routing consistent even when the output already matches
		return;
	}

	putFile(FB_BLANK_PATH, "4");
	clearFramebuffer();

	dispdbgSwitch(value ? DISP_SWITCH_HDMI_PARAM : DISP_SWITCH_LCD_PARAM);
	waitForOutput(value);

	// 1080p60 signal (mode 10): the DE scales the 720p fb up to it
	int fbW = HDMI_LOGICAL_WIDTH, fbH = HDMI_LOGICAL_HEIGHT, outW = 1920, outH = 1080;
	if (!value) {
		panelSize(&fbW, &fbH);
		outW = fbW;
		outH = fbH;
	}
	setFramebufferSize(fbW, fbH);
	commitLayerGeometry(fbW, fbH, outW, outH);
	usleep(250000);
	putFile(FB_BLANK_PATH, "0");
	commitLayerGeometry(fbW, fbH, outW, outH); // unblank can trigger another restore; re-verify

	// Re-enabling the LCD brings it up with whatever backlight/LUT state the
	// driver has, and the usual apply happened while the panel was disabled
	// (InitSettings runs before GFX_init/PLAT_initPlatform in nextui), so the
	// panel stays dark unless we re-apply here, after the switch.
	if (!value) {
		if (settings) {
			SetBrightness(GetBrightness());
			SetColortemp(GetColortemp());
			applyDisplayCalSettings();
		}
		else {
			// process hasn't mapped the settings shm yet (e.g. minarch races a
			// cable change during startup); a visible panel beats a dark one,
			// the correct value is re-applied on the next InitSettings
			SetRawBrightness(96);
		}
	}

	setHDMIAudioRoute(value);
}

///////// Platform specific scaling

int scaleVolume(int value) {
	return value * 5; // scale 0-20 to 0-100
}

int scaleBrightness(int value) {
	int raw;
	switch (value) {
		case 0: raw=4; break;
		case 1: raw=6; break;
		case 2: raw=10; break;
		case 3: raw=16; break;
		case 4: raw=32; break;
		case 5: raw=48; break;
		case 6: raw=64; break;
		case 7: raw=96; break;
		case 8: raw=128; break;
		case 9: raw=192; break;
		case 10: raw=255; break;
		default: raw=64; break;
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
		default: raw=0; break;
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
		default: raw=50; break;
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
		default: raw=50; break;
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
		default: raw=50; break;
	}
	return raw;
}

///////// Platform specific, unscaled accessors

#define DISP_LCD_SET_BRIGHTNESS  0x102
void SetRawBrightness(int val) { // 0 - 255
    int fd = open("/dev/disp", O_RDWR);
	if (fd >= 0) {
	    unsigned long param[4]={0,val,0,0};
		ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
		close(fd);
	}
}
void SetRawColortemp(int val) { // 0 - 255
	FILE *fd = fopen("/sys/class/disp/disp/attr/color_temperature", "w");
	if (fd) {
		fprintf(fd, "%i", val);
		fclose(fd);
	}
}

// Find the first A2DP playback volume control via amixer
static int get_a2dp_simple_control_name(char *buf, size_t buflen) {
    FILE *fp = popen("amixer -D bluealsa scontrols 2>/dev/null", "r");
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

static int get_usbc_card_num() {
	FILE *fp = popen("cat /proc/asound/cards", "r");
	if (!fp) return -1;

	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "audiocodec") == NULL) { // skip the built-in codec
			int card_num;
			if (sscanf(line, " %d ", &card_num) == 1) {
				pclose(fp);
				return card_num;
			}
		}
	}

	pclose(fp);
	return -1;
}

static int get_audiocodec_card_num() {
	FILE *fp = popen("cat /proc/asound/cards", "r");
	if (!fp) return -1;

	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "audiocodec") != NULL) { // look for the built-in codec
			int card_num;
			if (sscanf(line, " %d ", &card_num) == 1) {
				pclose(fp);
				return card_num;
			}
		}
	}

	pclose(fp);
	return -1;
}

void SetRawVolume(int val) { // in: 0-100
    if (GetAudioSink() == AUDIO_SINK_BLUETOOTH) {
        // bluealsa is a mixer plugin, not exposed as a separate card
        char ctl_name[128] = {0};
        int found = 0;
        for (int tries = 0; tries < 20; tries++) {
            if ((found = get_a2dp_simple_control_name(ctl_name, sizeof(ctl_name))))
                break;
            usleep(100000);
        }
        if (found) {
            char cmd[256];
            // Update volume on the device
            snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sset \"%s\" -M %d%% > /dev/null 2>&1", ctl_name, val);
            system(cmd);
			//printf("Set '%s' to %d%%\n", ctl_name, val); fflush(stdout);
        }
    } 
	else if (GetAudioSink() == AUDIO_SINK_USBDAC) {
		// USB DAC path: grab the first card that is not called "audiocodec"
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

        const unsigned int num_controls = mixer_get_num_ctls(mixer);
        for (unsigned int i = 0; i < num_controls; i++) {
            struct mixer_ctl *ctl = mixer_get_ctl(mixer, i);
            const char *name = mixer_ctl_get_name(ctl);
            if (!name) continue;

            if ((strstr(name, "PCM") || strstr(name, "Playback")) && (strstr(name, "Volume") || strstr(name, "volume"))) {
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
		// Speaker path: grab the card that is called "audiocodec"
		int card_num = get_audiocodec_card_num();
		if(card_num < 0) {
			card_num = 0; // fallback to card 0 if we can't find it
		}

		struct mixer *mixer = mixer_open(card_num);
        if (!mixer) {
            printf("Failed to open mixer\n"); fflush(stdout);
            return;
        }

        struct mixer_ctl *digital = mixer_get_ctl_by_name(mixer, "digital volume");
        if (digital) {
			mixer_ctl_set_percent(digital, 0, 100 - val); // reversed mapping
			//printf("Set 'digital volume' to %d%%\n", val); fflush(stdout);
		}

		struct mixer_ctl *lineout = mixer_get_ctl_by_name(mixer, "lineout volume");
        if (lineout) {
			mixer_ctl_set_percent(lineout, 0, val);
			//printf("Set 'lineout volume' to %d%%\n", val); fflush(stdout);
		}

		struct mixer_ctl *spk = mixer_get_ctl_by_name(mixer, "SPK");
		if (spk)
			mixer_ctl_set_value(spk, 0, val == 0 ? 0 : 1);

		struct mixer_ctl *lineout_switch = mixer_get_ctl_by_name(mixer, "LINEOUT");
		if (lineout_switch)
			mixer_ctl_set_value(lineout_switch, 0, val == 0 ? 0 : 1);

		mixer_close(mixer);
	}
}

// H700 lacks the enhance_* controls. TODO: implement these through the display LUT.
void SetRawContrast(int val){
	(void)val;
}
void SetRawSaturation(int val){
	(void)val;
}
void SetRawExposure(int val){
	(void)val;
}
void SetRawDisplayCal(int enabled, int red_gain, int green_gain, int blue_gain) {
	int ret = enabled
		? DisplayCal_enableWithValues(red_gain, green_gain, blue_gain)
		: DisplayCal_disable();
	if (ret != 0) {
		fprintf(stderr, "SetRawDisplayCal failed to %s display calibration: %i\n",
			enabled ? "enable" : "disable", ret);
		fflush(stderr);
	}
}

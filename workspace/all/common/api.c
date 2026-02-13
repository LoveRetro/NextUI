#include "defines.h"
#include "api.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <msettings.h>
#include <pthread.h>
#include <samplerate.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>

#include "utils.h"
#include "config.h"

#include <pthread.h>

extern pthread_mutex_t audio_mutex;

///////////////////////////////

void LOG_note(int level, const char *fmt, ...)
{
	char buf[1024] = {0};
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	switch (level)
	{
#ifdef DEBUG
	case LOG_DEBUG:
		printf("[DEBUG] %s", buf);
		fflush(stdout);
		break;
#endif
	case LOG_INFO:
		printf("[INFO] %s", buf);
		fflush(stdout);
		break;
	case LOG_WARN:
		fprintf(stderr, "[WARN] %s", buf);
		fflush(stderr);
		break;
	case LOG_ERROR:
		fprintf(stderr, "[ERROR] %s", buf);
		fflush(stderr);
		break;
	default:
		break;
	}
}

///////////////////////////////

uint32_t RGB_WHITE;
uint32_t RGB_BLACK;
uint32_t RGB_LIGHT_GRAY;
uint32_t RGB_GRAY;
uint32_t RGB_DARK_GRAY;

int lights_initialized = 0;
LightSettings lightsDefault[MAX_LIGHTS];
LightSettings lightsOff[MAX_LIGHTS];
LightSettings lightsCharging[MAX_LIGHTS];
LightSettings lightsLowBattery[MAX_LIGHTS];
LightSettings lightsCriticalBattery[MAX_LIGHTS];
LightSettings lightsSleep[MAX_LIGHTS];
LightSettings lightsAmbient[MAX_LIGHTS];
LightSettings (*lights)[MAX_LIGHTS] = NULL;

#define PROFILE_OVERRIDE_SIZE 4
int profile_override_top = -1;
enum LightProfile profile_override[PROFILE_OVERRIDE_SIZE];

///////////////////////////////

static struct GFX_Context
{
	SDL_Surface *screen;
	SDL_Surface *assets;

	int mode;
	int vsync;
} gfx;

static SDL_Rect asset_rects[ASSET_COUNT];
static uint32_t asset_rgbs[ASSET_COLORS];
GFX_Fonts font;

///////////////////////////////

uint32_t THEME_COLOR1;
uint32_t THEME_COLOR2;
uint32_t THEME_COLOR3;
uint32_t THEME_COLOR4;
uint32_t THEME_COLOR5;
uint32_t THEME_COLOR6;
uint32_t THEME_COLOR7;
SDL_Color ALT_BUTTON_TEXT_COLOR;

// move to utils?

// Function to convert hex color code to RGB and set the values
static inline uint32_t HexToUint(const char *hexColor)
{
	int r, g, b;
	sscanf(hexColor, "%02x%02x%02x", &r, &g, &b);
	return SDL_MapRGB(gfx.screen->format, r, g, b);
}

static inline uint32_t HexToUint32_unmapped(const char *hexColor)
{
	// Convert the hex string to an unsigned long
	uint32_t value = (uint32_t)strtoul(hexColor, NULL, 16);
	return value;
}

static inline void rgb_unpack(uint32_t col, int *r, int *g, int *b)
{
	*r = (col >> 16) & 0xff;
	*g = (col >> 8) & 0xff;
	*b = col & 0xff;
}

static inline uint32_t rgb_pack(int r, int g, int b)
{
	return (r << 16) + (g << 8) + b;
}

static inline uint32_t mapUint(uint32_t col)
{
	int r, g, b;
	rgb_unpack(col, &r, &g, &b);
	return SDL_MapRGB(gfx.screen->format, r, g, b);
}

static inline uint32_t UintMult(uint32_t color, uint32_t modulate_rgb)
{
	SDL_Color dest = uintToColour(color);
	SDL_Color modulate = uintToColour(modulate_rgb);

	dest.r = (int)dest.r * modulate.r / 255;
	dest.g = (int)dest.g * modulate.g / 255;
	dest.b = (int)dest.b * modulate.b / 255;

	return (dest.r << 16) | (dest.g << 8) | dest.b;
}

///////////////////////////////
static int qualityLevels[] = {
	3,
	4,
	2,
	1};
static struct PWR_Context
{
	int initialized;

	int can_sleep;
	int can_poweroff;
	int can_autosleep;
	int requested_sleep;
	int requested_wake;
	int resume_tick;

	pthread_t battery_pt;
	SDL_atomic_t is_charging;
	SDL_atomic_t charge;

	SDL_atomic_t is_online;
	SDL_atomic_t update_secs;
	SDL_atomic_t poll_network_status;
} pwr = {0};

static struct SND_Context
{
	int initialized;
	double frame_rate;

	int sample_rate_in;
	int sample_rate_out;

	SND_Frame *buffer;	// buf
	size_t frame_count; // buf_len

	int frame_in;	  // buf_w
	int frame_out;	  // buf_r
	int frame_filled; // max_buf_w

	int device_id; // SDL device id
} snd = {0};

///////////////////////////////

static int _;

static double current_fps = SCREEN_FPS;
static int fps_counter = 0;
PerfProfile perf = {0};

int currentshaderpass = 0;
int currentshadersrcw = 0;
int currentshadersrch = 0;
int currentshaderdstw = 0;
int currentshaderdsth = 0;
int currentshadertexw = 0;
int currentshadertexh = 0;

int should_rotate = 0;

FALLBACK_IMPLEMENTATION void PLAT_pinToCores(int core_type)
{
	// no-op
}

FALLBACK_IMPLEMENTATION void *PLAT_cpu_monitor(void *arg)
{
	return NULL;
}

FALLBACK_IMPLEMENTATION void PLAT_getCPUTemp()
{
	perf.cpu_temp = 0;
}

FALLBACK_IMPLEMENTATION void PLAT_getCPUSpeed()
{
	perf.cpu_speed = 0;
}

FALLBACK_IMPLEMENTATION void PLAT_getGPUTemp()
{
	perf.gpu_temp = 0;
}

FALLBACK_IMPLEMENTATION void PLAT_getGPUSpeed()
{
	perf.gpu_speed = 0;
}

FALLBACK_IMPLEMENTATION void PLAT_getGPUUsage()
{
	perf.gpu_usage = 0.0;
}

int GFX_loadSystemFont(const char *fontPath)
{
	// Load/Reload fonts
	if (!TTF_WasInit())
		TTF_Init();

	TTF_CloseFont(font.large);
	TTF_CloseFont(font.medium);
	TTF_CloseFont(font.small);
	TTF_CloseFont(font.tiny);
	TTF_CloseFont(font.micro);

	font.large = TTF_OpenFont(fontPath, SCALE1(FONT_LARGE));
	font.medium = TTF_OpenFont(fontPath, SCALE1(FONT_MEDIUM));
	font.small = TTF_OpenFont(fontPath, SCALE1(FONT_SMALL));
	font.tiny = TTF_OpenFont(fontPath, SCALE1(FONT_TINY));
	font.micro = TTF_OpenFont(fontPath, SCALE1(FONT_MICRO));

	TTF_SetFontStyle(font.large, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.medium, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.small, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.tiny, TTF_STYLE_BOLD);
	TTF_SetFontStyle(font.micro, TTF_STYLE_BOLD);

	return 0;
}

int GFX_updateColors(void)
{
	// We are currently micro managing all of these screen-mapped colors,
	// should just move this to the caller.
	THEME_COLOR1 = mapUint(CFG_getColor(1));
	THEME_COLOR2 = mapUint(CFG_getColor(2));
	THEME_COLOR3 = mapUint(CFG_getColor(3));
	THEME_COLOR4 = mapUint(CFG_getColor(4));
	THEME_COLOR5 = mapUint(CFG_getColor(5));
	THEME_COLOR6 = mapUint(CFG_getColor(6));
	THEME_COLOR7 = mapUint(CFG_getColor(7));
	ALT_BUTTON_TEXT_COLOR = uintToColour(CFG_getColor(3));

	return 0;
}

SDL_Surface *GFX_init(int mode)
{
	// Platform-specific init
	// This might affect FIXED_SCALE, so do it first
	PLAT_initPlatform();

	gfx.screen = PLAT_initVideo();
	gfx.vsync = VSYNC_STRICT;
	gfx.mode = mode;

	// TODO: all this doesn't really belong here...
	// tried adding to PWR_init() but that was no good (not sure why)
	
	CFG_init(GFX_loadSystemFont, GFX_updateColors);

	// We always have to symlink, does not depend on NTP being enabled
	PLAT_initTimezones();
	PLAT_setCurrentTimezone(PLAT_getCurrentTimezone());

	PLAT_initLid();
	LEDS_initLeds();

	RGB_WHITE = SDL_MapRGB(gfx.screen->format, TRIAD_WHITE);
	RGB_BLACK = SDL_MapRGB(gfx.screen->format, TRIAD_BLACK);
	RGB_LIGHT_GRAY = SDL_MapRGB(gfx.screen->format, TRIAD_LIGHT_GRAY);
	RGB_GRAY = SDL_MapRGB(gfx.screen->format, TRIAD_GRAY);
	RGB_DARK_GRAY = SDL_MapRGB(gfx.screen->format, TRIAD_DARK_GRAY);

	asset_rgbs[ASSET_WHITE_PILL] = RGB_WHITE;
	asset_rgbs[ASSET_BLACK_PILL] = RGB_BLACK;
	asset_rgbs[ASSET_DARK_GRAY_PILL] = RGB_DARK_GRAY;
	asset_rgbs[ASSET_OPTION] = RGB_DARK_GRAY;
	asset_rgbs[ASSET_BUTTON] = RGB_WHITE;
	asset_rgbs[ASSET_PAGE_BG] = RGB_WHITE;
	asset_rgbs[ASSET_STATE_BG] = RGB_WHITE;
	asset_rgbs[ASSET_PAGE] = RGB_BLACK;
	asset_rgbs[ASSET_BAR] = RGB_WHITE;
	asset_rgbs[ASSET_BAR_BG] = RGB_WHITE;
	asset_rgbs[ASSET_BAR_BG_MENU] = RGB_WHITE;
	asset_rgbs[ASSET_UNDERLINE] = RGB_GRAY;
	asset_rgbs[ASSET_DOT] = RGB_LIGHT_GRAY;
	asset_rgbs[ASSET_HOLE] = RGB_BLACK;

	asset_rects[ASSET_WHITE_PILL] = (SDL_Rect){SCALE4(1, 1, 30, 30)};
	asset_rects[ASSET_BLACK_PILL] = (SDL_Rect){SCALE4(33, 1, 30, 30)};
	asset_rects[ASSET_DARK_GRAY_PILL] = (SDL_Rect){SCALE4(65, 1, 30, 30)};
	asset_rects[ASSET_OPTION] = (SDL_Rect){SCALE4(97, 1, 20, 20)};
	asset_rects[ASSET_BUTTON] = (SDL_Rect){SCALE4(1, 33, 20, 20)};
	asset_rects[ASSET_PAGE_BG] = (SDL_Rect){SCALE4(64, 33, 15, 15)};
	asset_rects[ASSET_STATE_BG] = (SDL_Rect){SCALE4(23, 54, 8, 8)};
	asset_rects[ASSET_PAGE] = (SDL_Rect){SCALE4(39, 54, 6, 6)};
	asset_rects[ASSET_BAR] = (SDL_Rect){SCALE4(33, 58, 4, 4)};
	asset_rects[ASSET_BAR_BG] = (SDL_Rect){SCALE4(33, 58, 4, 4)};
	asset_rects[ASSET_BAR_BG_MENU] = (SDL_Rect){SCALE4(33, 58, 4, 4)};
	asset_rects[ASSET_UNDERLINE] = (SDL_Rect){SCALE4(85, 51, 3, 3)};
	asset_rects[ASSET_DOT] = (SDL_Rect){SCALE4(33, 54, 2, 2)};
	asset_rects[ASSET_BRIGHTNESS] = (SDL_Rect){SCALE4(1, 85, 19, 19)};
	asset_rects[ASSET_COLORTEMP] = (SDL_Rect){SCALE4(41, 85, 9, 19)};
	asset_rects[ASSET_VOLUME_MUTE] = (SDL_Rect){SCALE4(21, 85, 10, 19)};
	asset_rects[ASSET_VOLUME] = (SDL_Rect){SCALE4(21, 85, 19, 19)};
	asset_rects[ASSET_BATTERY] = (SDL_Rect){SCALE4(47, 51, 17, 10)};
	asset_rects[ASSET_BATTERY_LOW] = (SDL_Rect){SCALE4(66, 51, 17, 10)};
	asset_rects[ASSET_BATTERY_FILL] = (SDL_Rect){SCALE4(81, 33, 12, 6)};
	asset_rects[ASSET_BATTERY_FILL_LOW] = (SDL_Rect){SCALE4(1, 55, 12, 6)};
	asset_rects[ASSET_BATTERY_BOLT] = (SDL_Rect){SCALE4(81, 41, 12, 6)};
	asset_rects[ASSET_SCROLL_UP] = (SDL_Rect){SCALE4(97, 23, 24, 6)};
	asset_rects[ASSET_SCROLL_DOWN] = (SDL_Rect){SCALE4(97, 31, 24, 6)};
	asset_rects[ASSET_WIFI] = (SDL_Rect){SCALE4(1, 104, 12, 12)};
	asset_rects[ASSET_WIFI_MED] = (SDL_Rect){SCALE4(14, 104, 12, 12)};
	asset_rects[ASSET_WIFI_LOW] = (SDL_Rect){SCALE4(27, 104, 12, 12)};
	asset_rects[ASSET_WIFI_OFF] = (SDL_Rect){SCALE4(40, 104, 12, 12)};
	asset_rects[ASSET_CHECKCIRCLE] = (SDL_Rect){SCALE4(1, 117, 10, 10)};
	asset_rects[ASSET_LOCK] = (SDL_Rect){SCALE4(12, 116, 8, 11)};
	asset_rects[ASSET_HOLE] = (SDL_Rect){SCALE4(1, 63, 20, 20)};
	asset_rects[ASSET_GAMEPAD] = (SDL_Rect){SCALE4(91, 51, 17, 10)};
	asset_rects[ASSET_SETTINGS] = (SDL_Rect){SCALE4(21, 117, 10, 10)};
	asset_rects[ASSET_STORE] = (SDL_Rect){SCALE4(66, 117, 10, 10)};
	asset_rects[ASSET_POWEROFF] = (SDL_Rect){SCALE4(43, 117, 10, 10)};
	asset_rects[ASSET_SUSPEND] = (SDL_Rect){SCALE4(32, 117, 10, 10)};
	asset_rects[ASSET_RESTART] = (SDL_Rect){SCALE4(54, 119, 11, 8)};
	asset_rects[ASSET_BLUETOOTH] = (SDL_Rect){SCALE4(53, 104, 12, 12)};
	asset_rects[ASSET_BLUETOOTH_OFF] = (SDL_Rect){SCALE4(66, 104, 12, 12)};
	asset_rects[ASSET_AUDIO] = (SDL_Rect){SCALE4(79, 104, 12, 12)};
	asset_rects[ASSET_CONTROLLER] = (SDL_Rect){SCALE4(92, 104, 12, 12)};

	char asset_path[MAX_PATH];
	sprintf(asset_path, RES_PATH "/assets@%ix.png", FIXED_SCALE);
	if (!exists(asset_path))
		LOG_info("missing assets, you're about to segfault dummy!\n");
	gfx.assets = IMG_Load(asset_path);

	PLAT_clearAll();

	return gfx.screen;
}
void GFX_quit(void)
{

	TTF_CloseFont(font.large);
	TTF_CloseFont(font.medium);
	TTF_CloseFont(font.small);
	TTF_CloseFont(font.tiny);
	TTF_CloseFont(font.micro);

	SDL_FreeSurface(gfx.assets);

	CFG_quit();

	GFX_freeAAScaler();

	PLAT_quitVideo();
}

void GFX_setMode(int mode)
{
	gfx.mode = mode;
}
int GFX_getVsync(void)
{
	return gfx.vsync;
}
void GFX_setVsync(int vsync)
{
	PLAT_setVsync(vsync);
	gfx.vsync = vsync;
}

int GFX_hdmiChanged(void)
{
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi == -1)
		had_hdmi = has_hdmi;
	if (had_hdmi == has_hdmi)
		return 0;
	had_hdmi = has_hdmi;
	return 1;
}

#define FRAME_BUDGET 17 // 60fps
static uint32_t frame_start = 0;

static uint64_t per_frame_start = 0;
#define FPS_BUFFER_SIZE 50
// filling with  60.1 cause i'd rather underrun than overflow in start phase
static double fps_buffer[FPS_BUFFER_SIZE] = {60.1};
static double frame_time_buffer[FPS_BUFFER_SIZE] = {0};
static int fps_buffer_index = 0;

void GFX_startFrame(void)
{
	frame_start = SDL_GetTicks();
}

uint32_t GFX_extract_average_color(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (!data)
	{
		return 0;
	}

	const uint16_t *pixels = (const uint16_t *)data;
	int pixel_count = width * height;

	uint64_t total_r = 0;
	uint64_t total_g = 0;
	uint64_t total_b = 0;
	uint32_t colorful_pixel_count = 0;

	for (unsigned y = 0; y < height; y++)
	{
		for (unsigned x = 0; x < width; x++)
		{
			uint16_t pixel = pixels[y * (pitch / 2) + x];

			uint8_t r = ((pixel & 0xF800) >> 11) << 3;
			uint8_t g = ((pixel & 0x07E0) >> 5) << 2;
			uint8_t b = (pixel & 0x001F) << 3;

			r |= r >> 5;
			g |= g >> 6;
			b |= b >> 5;

			uint8_t max_c = fmaxf(fmaxf(r, g), b);
			uint8_t min_c = fminf(fminf(r, g), b);
			uint8_t saturation = max_c == 0 ? 0 : (max_c - min_c) * 255 / max_c;

			if (saturation > 50 && max_c > 50)
			{
				total_r += r;
				total_g += g;
				total_b += b;
				colorful_pixel_count++;
			}
		}
	}

	if (colorful_pixel_count == 0)
	{

		colorful_pixel_count = pixel_count;
		total_r = total_g = total_b = 0;
		for (unsigned y = 0; y < height; y++)
		{
			for (unsigned x = 0; x < width; x++)
			{
				uint16_t pixel = pixels[y * (pitch / 2) + x];
				uint8_t r = ((pixel & 0xF800) >> 11) << 3;
				uint8_t g = ((pixel & 0x07E0) >> 5) << 2;
				uint8_t b = (pixel & 0x001F) << 3;

				r |= r >> 5;
				g |= g >> 6;
				b |= b >> 5;

				total_r += r;
				total_g += g;
				total_b += b;
			}
		}
	}

	uint8_t avg_r = total_r / colorful_pixel_count;
	uint8_t avg_g = total_g / colorful_pixel_count;
	uint8_t avg_b = total_b / colorful_pixel_count;

	return (avg_r << 16) | (avg_g << 8) | avg_b;
}

void GFX_setAmbientColor(const void *data, unsigned width, unsigned height, size_t pitch, int mode)
{
	if (MAX_LIGHTS <= 0 || mode == 0)
		return;

	uint32_t dominant_color = GFX_extract_average_color(data, width, height, pitch);

	if (mode == 1 || mode == 2 || mode == 5)
	{
		(lightsAmbient)[2].color1 = dominant_color;
		(lightsAmbient)[2].effect = 4;
		(lightsAmbient)[2].brightness = 100;
	}
	if (mode == 1 || mode == 3)
	{
		(lightsAmbient)[0].color1 = dominant_color;
		(lightsAmbient)[0].effect = 4;
		(lightsAmbient)[0].brightness = 100;
		(lightsAmbient)[1].color1 = dominant_color;
		(lightsAmbient)[1].effect = 4;
		(lightsAmbient)[1].brightness = 100;
	}
	if (mode == 1 || mode == 4 || mode == 5)
	{
		(lightsAmbient)[3].color1 = dominant_color;
		(lightsAmbient)[3].effect = 4;
		(lightsAmbient)[3].brightness = 100;
	}
}

void GFX_flip(SDL_Surface *screen)
{
	{
		uint64_t performance_frequency = SDL_GetPerformanceFrequency();
		uint64_t frame_duration = SDL_GetPerformanceCounter() - per_frame_start;
		double elapsed_time_s = (double)frame_duration / performance_frequency;
		double frame_ms = elapsed_time_s * 1000.0;
		//LOG_info("GFX_flip: Frame time before flip: %.2f ms\n", frame_ms);
	}
	PLAT_flip(screen, 0);

	perf.fps = current_fps;
	fps_counter++;

	uint64_t performance_frequency = SDL_GetPerformanceFrequency();
	uint64_t frame_duration = SDL_GetPerformanceCounter() - per_frame_start;
	double elapsed_time_s = (double)frame_duration / performance_frequency;
	double tempfps = 1.0 / elapsed_time_s;

	// Stats logic
	double frame_ms = elapsed_time_s * 1000.0;
	double target_ms = 1000.0 / SCREEN_FPS;
	perf.jitter = fabs(frame_ms - target_ms);
	
	if (frame_ms > target_ms * 1.1) {
		perf.frame_drops++;
		//LOG_warn("GFX_flip: Frame drop detected! Frame time: %.2f ms (target: %.2f ms)\n", frame_ms, target_ms);
	}

	if (tempfps < SCREEN_FPS * 0.8 || tempfps > SCREEN_FPS * 1.2)
		tempfps = SCREEN_FPS;

	fps_buffer[fps_buffer_index] = tempfps;
	frame_time_buffer[fps_buffer_index] = frame_ms;
	fps_buffer_index = (fps_buffer_index + 1) % FPS_BUFFER_SIZE;
	// give it a little bit to stabilize and then use, meanwhile the buffer will
	// cover it
	if (fps_counter > 100)
	{
		double average_fps = 0.0;
		double avg_ft = 0.0;
		double max_ft = 0.0;
		int fpsbuffersize = MIN(fps_counter, FPS_BUFFER_SIZE);
		for (int i = 0; i < fpsbuffersize; i++)
		{
			average_fps += fps_buffer[i];
			avg_ft += frame_time_buffer[i];
			if (frame_time_buffer[i] > max_ft) max_ft = frame_time_buffer[i];
		}
		average_fps /= fpsbuffersize;
		avg_ft /= fpsbuffersize;
		current_fps = average_fps;
		perf.avg_frame_ms = avg_ft;
		perf.max_frame_ms = max_ft;
	}

	per_frame_start = SDL_GetPerformanceCounter();
}
void GFX_GL_Swap()
{
	{
		uint64_t performance_frequency = SDL_GetPerformanceFrequency();
		uint64_t frame_duration = SDL_GetPerformanceCounter() - per_frame_start;
		double elapsed_time_s = (double)frame_duration / performance_frequency;
		double frame_ms = elapsed_time_s * 1000.0;
		//LOG_info("GFX_GL_Swap: Frame time before flip: %.2f ms\n", frame_ms);
	}
	PLAT_GL_Swap();

	perf.fps = current_fps;
	fps_counter++;

	uint64_t performance_frequency = SDL_GetPerformanceFrequency();
	uint64_t frame_duration = SDL_GetPerformanceCounter() - per_frame_start;
	double elapsed_time_s = (double)frame_duration / performance_frequency;
	double tempfps = 1.0 / elapsed_time_s;

	// Stats logic
	double frame_ms = elapsed_time_s * 1000.0;
	double target_ms = 1000.0 / SCREEN_FPS;
	perf.jitter = fabs(frame_ms - target_ms);
	
	if (frame_ms > target_ms * 1.1) {
		perf.frame_drops++;
		//LOG_warn("GFX_GL_Swap: Frame drop detected! Frame time: %.2f ms (target: %.2f ms)\n", frame_ms, target_ms);
	}

	if (tempfps < SCREEN_FPS * 0.8 || tempfps > SCREEN_FPS * 1.2)
		tempfps = SCREEN_FPS;

	fps_buffer[fps_buffer_index] = tempfps;
	frame_time_buffer[fps_buffer_index] = frame_ms;
	fps_buffer_index = (fps_buffer_index + 1) % FPS_BUFFER_SIZE;
	// give it a little bit to stabilize and then use, meanwhile the buffer will
	// cover it
	if (fps_counter > 100)
	{
		double average_fps = 0.0;
		double avg_ft = 0.0;
		double max_ft = 0.0;
		int fpsbuffersize = MIN(fps_counter, FPS_BUFFER_SIZE);
		for (int i = 0; i < fpsbuffersize; i++)
		{
			average_fps += fps_buffer[i];
			avg_ft += frame_time_buffer[i];
			if (frame_time_buffer[i] > max_ft) max_ft = frame_time_buffer[i];
		}
		average_fps /= fpsbuffersize;
		avg_ft /= fpsbuffersize;
		current_fps = average_fps;
		perf.avg_frame_ms = avg_ft;
		perf.max_frame_ms = max_ft;
	}

	per_frame_start = SDL_GetPerformanceCounter();
}
// eventually this function should be removed as its only here because of all the audio buffer based delay stuff
void GFX_sync(void)
{
	uint32_t frame_duration = SDL_GetTicks() - frame_start;
	if (gfx.vsync != VSYNC_OFF)
	{
		// this limiting condition helps SuperFX chip games
		if (gfx.vsync == VSYNC_STRICT || frame_start == 0 || frame_duration < FRAME_BUDGET)
		{ // only wait if we're under frame budget
			PLAT_vsync(FRAME_BUDGET - frame_duration);
		}
	}
	else
	{
		if (frame_duration < FRAME_BUDGET)
			SDL_Delay(FRAME_BUDGET - frame_duration);
	}
}

void GFX_flip_fixed_rate(SDL_Surface *screen, double target_fps)
{
	if (target_fps == 0.0)
		target_fps = SCREEN_FPS;
	double frame_budget_ms = 1000.0 / target_fps;

	static int64_t frame_index = -1;
	static int64_t first_frame_start_time = 0;
	static double last_target_fps = 0.0;

	int64_t perf_freq = SDL_GetPerformanceFrequency();
	int64_t now = SDL_GetPerformanceCounter();

	if (++frame_index == 0 || target_fps != last_target_fps)
	{
		frame_index = 0;
		first_frame_start_time = now;
		last_target_fps = target_fps;
	}

	int64_t frame_duration = perf_freq / target_fps;
	int64_t time_of_frame = first_frame_start_time + frame_index * frame_duration;
	int64_t offset = now - time_of_frame;
	const int max_lost_frames = 2;

	// printf("%s: frame #%lld, time is %lld, scheduled at %lld, offset is %lld\n",
	// 	__FUNCTION__,
	// 	frame_index,
	// 	now,
	// 	time_of_frame,
	// 	now - time_of_frame);

	if (offset > 0)
	{
		if (offset > max_lost_frames * frame_duration)
		{
			frame_index = -1;
			last_target_fps = 0.0;
			LOG_debug("%s: lost sync by more than %d frames (late) @%llu -> reset\n\n", __FUNCTION__, max_lost_frames, SDL_GetPerformanceCounter());
		}
	}
	else
	{
		if (offset < -max_lost_frames * frame_duration)
		{
			frame_index = -1;
			last_target_fps = 0.0;
			LOG_debug("%s: lost sync by more than %d frames (early ?!) @%llu -> reset\n\n", __FUNCTION__, max_lost_frames, SDL_GetPerformanceCounter());
		}
		else if (offset < 0)
		{
			useconds_t time_to_sleep_us = (useconds_t)((time_of_frame - now) * 1e6 / perf_freq);

			// The OS scheduling algorithm cannot guarantee that
			// the sleep will last the exact amount of requested time.
			// We sleep as much as we can using the OS primitive.
			const useconds_t min_waiting_time = 2000;
			if (time_to_sleep_us > min_waiting_time)
			{
				usleep(time_to_sleep_us - min_waiting_time);
			}

			while (SDL_GetPerformanceCounter() < time_of_frame)
			{
				// nothing...
			}
		}
	}
	PLAT_GL_Swap();

	double elapsed_time_s = (double)(SDL_GetPerformanceCounter() - per_frame_start) / perf_freq;
	double tempfps = 1.0 / elapsed_time_s;

	// Stats logic
	double frame_ms = elapsed_time_s * 1000.0;
	double target_ms = 1000.0 / target_fps;
	perf.jitter = fabs(frame_ms - target_ms);
	
	if (frame_ms > target_ms * 1.1) {
		perf.frame_drops++;
		//LOG_warn("GFX_flip_fixed_rate: Frame drop detected! Frame time: %.2f ms (target: %.2f ms)\n", frame_ms, target_ms);
	}

	fps_buffer[fps_buffer_index] = tempfps;
	frame_time_buffer[fps_buffer_index] = frame_ms;
	fps_buffer_index = (fps_buffer_index + 1) % FPS_BUFFER_SIZE;
	// give it a little bit to stabilize and then use, meanwhile the buffer will
	// cover it
	if (fps_counter++ > 100)
	{
		double average_fps = 0.0;
		double avg_ft = 0.0;
		double max_ft = 0.0;
		int fpsbuffersize = MIN(fps_counter, FPS_BUFFER_SIZE);
		for (int i = 0; i < fpsbuffersize; i++)
		{
			average_fps += fps_buffer[i];
			avg_ft += frame_time_buffer[i];
			if (frame_time_buffer[i] > max_ft) max_ft = frame_time_buffer[i];
		}
		average_fps /= fpsbuffersize;
		avg_ft /= fpsbuffersize;
		current_fps = average_fps;
		perf.fps = current_fps;
		perf.avg_frame_ms = avg_ft;
		perf.max_frame_ms = max_ft;
	}
	else
	{
		current_fps = target_fps;
		perf.fps = target_fps;
		perf.avg_frame_ms = 1000.0 / target_fps;
		perf.max_frame_ms = perf.avg_frame_ms;
	}
	per_frame_start = SDL_GetPerformanceCounter();
}

// if a fake vsycn delay is really needed
void GFX_delay(void)
{
	uint32_t frame_duration = SDL_GetTicks() - frame_start;
	if (frame_duration < ((1 / SCREEN_FPS) * 1000))
		SDL_Delay(((1 / SCREEN_FPS) * 1000) - frame_duration);
}

FALLBACK_IMPLEMENTATION int PLAT_supportsOverscan(void) { return 0; }
FALLBACK_IMPLEMENTATION void PLAT_setEffectColor(int next_color) {}

int GFX_truncateText(TTF_Font *font, const char *in_name, char *out_name, int max_width, int padding)
{
	int text_width;
	strcpy(out_name, in_name);
	TTF_SizeUTF8(font, out_name, &text_width, NULL);
	text_width += padding;

	while (text_width > max_width)
	{
		int len = strlen(out_name);
		strcpy(&out_name[len - 4], "...\0");
		TTF_SizeUTF8(font, out_name, &text_width, NULL);
		text_width += padding;
	}

	return text_width;
}
int GFX_getTextHeight(TTF_Font *font, const char *in_name, char *out_name, int max_width, int padding)
{
	int text_height;
	strcpy(out_name, in_name);
	TTF_SizeUTF8(font, out_name, NULL, &text_height);
	text_height += padding;

	return text_height;
}
int GFX_getTextWidth(TTF_Font *font, const char *in_name, char *out_name, int max_width, int padding)
{
	int text_width;
	strcpy(out_name, in_name);
	TTF_SizeUTF8(font, out_name, &text_width, NULL);
	text_width += padding;

	return text_width;
}

int GFX_wrapText(TTF_Font *font, char *str, int max_width, int max_lines)
{
	if (!str)
		return 0;

	int line_width;
	int max_line_width = 0;
	char *line = str;
	char buffer[MAX_PATH];

	TTF_SizeUTF8(font, line, &line_width, NULL);
	if (line_width <= max_width)
	{
		line_width = GFX_truncateText(font, line, buffer, max_width, 0);
		strcpy(line, buffer);
		return line_width;
	}

	char *prev = NULL;
	char *tmp = line;
	int lines = 1;
	int i = 0;
	while (!max_lines || lines < max_lines)
	{
		tmp = strchr(tmp, ' ');
		if (!tmp)
		{
			if (prev)
			{
				TTF_SizeUTF8(font, line, &line_width, NULL);
				if (line_width >= max_width)
				{
					if (line_width > max_line_width)
						max_line_width = line_width;
					prev[0] = '\n';
					line = prev + 1;
				}
			}
			break;
		}
		tmp[0] = '\0';

		TTF_SizeUTF8(font, line, &line_width, NULL);

		if (line_width >= max_width)
		{ // wrap
			if (line_width > max_line_width)
				max_line_width = line_width;
			tmp[0] = ' ';
			tmp += 1;
			prev[0] = '\n';
			prev += 1;
			line = prev;
			lines += 1;
		}
		else
		{ // continue
			tmp[0] = ' ';
			prev = tmp;
			tmp += 1;
		}
		i += 1;
	}

	line_width = GFX_truncateText(font, line, buffer, max_width, 0);
	strcpy(line, buffer);

	if (line_width > max_line_width)
		max_line_width = line_width;
	return max_line_width;
}

int GFX_blitWrappedText(TTF_Font *font, const char *text, int max_width, int max_lines, SDL_Color color, SDL_Surface *surface, int y)
{
	if (!text || !text[0])
		return y;

	int center_x = surface->w / 2;

	char *text_copy = strdup(text);
	if (!text_copy)
		return y;

	char *words[256];
	int word_count = 0;

	// Split text into words
	char *token = strtok(text_copy, " ");
	while (token && word_count < 256) {
		words[word_count++] = token;
		token = strtok(NULL, " ");
	}

	// Build and render lines
	char line[512] = "";
	int line_num = 0;
	for (int w = 0; w < word_count; w++) {
		char test_line[512];
		if (line[0] == '\0') {
			snprintf(test_line, sizeof(test_line), "%s", words[w]);
		} else {
			snprintf(test_line, sizeof(test_line), "%s %s", line, words[w]);
		}

		int test_width, test_height;
		TTF_SizeUTF8(font, test_line, &test_width, &test_height);

		if (test_width > max_width && line[0] != '\0') {
			// Current line is full
			if (!max_lines || line_num < max_lines - 1) {
				// Render line and continue to next
				SDL_Surface *line_surface = TTF_RenderUTF8_Blended(font, line, color);
				if (line_surface) {
					SDL_BlitSurface(line_surface, NULL, surface, &(SDL_Rect){
						center_x - line_surface->w / 2, y
					});
					y += line_surface->h;
					SDL_FreeSurface(line_surface);
				}
				line_num++;
				snprintf(line, sizeof(line), "%s", words[w]);
			} else {
				// Last allowed line with more words remaining - add ellipsis
				char truncated[512];
				snprintf(truncated, sizeof(truncated), "%s...", line);
				SDL_Surface *line_surface = TTF_RenderUTF8_Blended(font, truncated, color);
				if (line_surface) {
					SDL_BlitSurface(line_surface, NULL, surface, &(SDL_Rect){
						center_x - line_surface->w / 2, y
					});
					y += line_surface->h;
					SDL_FreeSurface(line_surface);
				}
				line[0] = '\0';  // Mark as rendered
				break;
			}
		} else {
			snprintf(line, sizeof(line), "%s", test_line);
		}
	}

	// Render any remaining text
	if (line[0] != '\0') {
		SDL_Surface *line_surface = TTF_RenderUTF8_Blended(font, line, color);
		if (line_surface) {
			SDL_BlitSurface(line_surface, NULL, surface, &(SDL_Rect){
				center_x - line_surface->w / 2, y
			});
			y += line_surface->h;
			SDL_FreeSurface(line_surface);
		}
	}

	free(text_copy);
	return y;
}

///////////////////////////////

// scale_blend (and supporting logic) from picoarch

struct blend_args
{
	int w_ratio_in;
	int w_ratio_out;
	uint16_t w_bp[2];
	int h_ratio_in;
	int h_ratio_out;
	uint16_t h_bp[2];
	uint16_t *blend_line;
} blend_args;

// Pure C fallbacks
static inline uint32_t average16_c(uint32_t c1, uint32_t c2)
{
	return (c1 + c2 + ((c1 ^ c2) & 0x0821)) >> 1;
}

static inline uint32_t average32_c(uint32_t c1, uint32_t c2)
{
	uint32_t sum = c1 + c2;
	uint32_t ret = sum + ((c1 ^ c2) & 0x08210821);
	uint32_t of = ((sum < c1) | (ret < sum)) << 31;

	return (ret >> 1) | of;
}

// not sure we are activating this anywhere currently, but we could.
// to be honest, I'm not sure if we're using this function at all right now,
// but I'm fixing it anyway so might as well improve it.
#ifdef HAS_NEON
// #if defined(__ARM_NEON) || defined(__ARM_NEON__)
static inline uint32x4_t average32_neon(uint32x4_t a, uint32x4_t b)
{
	return vhaddq_u32(a, b); // vector halving add (a + b) >> 1
}
#endif

// aarch32 asm
#if defined(__arm__) && !defined(__aarch64__)
static inline uint32_t average16(uint32_t c1, uint32_t c2)
{
	uint32_t ret, lowbits = 0x0821;
	asm volatile(
		"eor %0, %2, %3\n\t"
		"and %0, %0, %1\n\t"
		"add %0, %3, %0\n\t"
		"add %0, %0, %2\n\t"
		"lsr %0, %0, #1\n\t"
		: "=&r"(ret)
		: "r"(lowbits), "r"(c1), "r"(c2));
	return ret;
}

static inline uint32_t average32(uint32_t c1, uint32_t c2)
{
	uint32_t ret, lowbits = 0x08210821;
	asm volatile(
		"eor %0, %3, %1\n\t"
		"and %0, %0, %2\n\t"
		"adds %0, %1, %0\n\t"
		"and %1, %1, #0\n\t"
		"movcs %1, #0x80000000\n\t"
		"adds %0, %0, %3\n\t"
		"rrx %0, %0\n\t"
		"orr %0, %0, %1\n\t"
		: "=&r"(ret), "+r"(c2)
		: "r"(lowbits), "r"(c1)
		: "cc");
	return ret;
}

// aarch64 asm
#elif defined(__aarch64__)

static inline uint32_t average16(uint32_t c1, uint32_t c2)
{
	uint32_t result;
	asm volatile(
		"and w2, %w0, %w1\n\t"
		"eor w3, %w0, %w1\n\t"
		"lsr w3, w3, #1\n\t"
		"add %w2, w2, w3\n\t"
		"mov %w0, w2\n\t"
		: "+r"(c1)
		: "r"(c2)
		: "w2", "w3");
	return c1;
}

static inline uint32_t average32(uint32_t c1, uint32_t c2)
{
	uint32_t result;
	asm volatile(
		"and w2, %w0, %w1\n\t"
		"eor w3, %w0, %w1\n\t"
		"lsr w3, w3, #1\n\t"
		"add w2, w2, w3\n\t"
		"mov %w0, w2\n\t"
		: "+r"(c1)
		: "r"(c2)
		: "w2", "w3");
	return c1;
}

// fallback if nothing else works
#else
#define average16 average16_c
#define average32 average32_c
#endif

#define AVERAGE16_NOCHK(c1, c2) (average16((c1), (c2)))
#define AVERAGE32_NOCHK(c1, c2) (average32((c1), (c2)))

#define AVERAGE16(c1, c2) ((c1) == (c2) ? (c1) : AVERAGE16_NOCHK((c1), (c2)))
#define AVERAGE16_1_3(c1, c2) ((c1) == (c2) ? (c1) : (AVERAGE16_NOCHK(AVERAGE16_NOCHK((c1), (c2)), (c2))))

#define AVERAGE32(c1, c2) ((c1) == (c2) ? (c1) : AVERAGE32_NOCHK((c1), (c2)))
#define AVERAGE32_1_3(c1, c2) ((c1) == (c2) ? (c1) : (AVERAGE32_NOCHK(AVERAGE32_NOCHK((c1), (c2)), (c2))))

static inline int gcd(int a, int b)
{
	return b ? gcd(b, a % b) : a;
}

static void scaleAA(void *__restrict src, void *__restrict dst, uint32_t w, uint32_t h, uint32_t pitch, uint32_t dst_w, uint32_t dst_h, uint32_t dst_p)
{
	int dy = 0;
	int lines = h;

	int rat_w = blend_args.w_ratio_in;
	int rat_dst_w = blend_args.w_ratio_out;
	uint16_t *bw = blend_args.w_bp;

	int rat_h = blend_args.h_ratio_in;
	int rat_dst_h = blend_args.h_ratio_out;
	uint16_t *bh = blend_args.h_bp;

	while (lines--)
	{
		while (dy < rat_dst_h)
		{
			uint16_t *dst16 = (uint16_t *)dst;
			uint16_t *pblend = (uint16_t *)blend_args.blend_line;
			int col = w;
			int dx = 0;

			uint16_t *pnext = (uint16_t *)(src + pitch);
			if (!lines)
				pnext -= (pitch / sizeof(uint16_t));

			if (dy > rat_dst_h - bh[0])
			{
				pblend = pnext;
			}
			else if (dy <= bh[0])
			{
				/* Drops const, won't get touched later though */
				pblend = (uint16_t *)src;
			}
			else
			{
				const uint32_t *src32 = (const uint32_t *)src;
				const uint32_t *pnext32 = (const uint32_t *)pnext;
				uint32_t *pblend32 = (uint32_t *)pblend;
				int count = w / 2;

				if (dy <= bh[1])
				{
					const uint32_t *tmp = pnext32;
					pnext32 = src32;
					src32 = tmp;
				}

				if (dy > rat_dst_h - bh[1] || dy <= bh[1])
				{
					while (count--)
					{
						*pblend32++ = AVERAGE32_1_3(*src32, *pnext32);
						src32++;
						pnext32++;
					}
				}
				else
				{
					while (count--)
					{
						*pblend32++ = AVERAGE32(*src32, *pnext32);
						src32++;
						pnext32++;
					}
				}
			}

			while (col--)
			{
				uint16_t a, b, out;

				a = *pblend;
				b = *(pblend + 1);

				while (dx < rat_dst_w)
				{
					if (a == b)
					{
						out = a;
					}
					else if (dx > rat_dst_w - bw[0])
					{ // top quintile, bbbb
						out = b;
					}
					else if (dx <= bw[0])
					{ // last quintile, aaaa
						out = a;
					}
					else
					{
						if (dx > rat_dst_w - bw[1])
						{ // 2nd quintile, abbb
							a = AVERAGE16_NOCHK(a, b);
						}
						else if (dx <= bw[1])
						{ // 4th quintile, aaab
							b = AVERAGE16_NOCHK(a, b);
						}

						out = AVERAGE16_NOCHK(a, b); // also 3rd quintile, aabb
					}
					*dst16++ = out;
					dx += rat_w;
				}

				dx -= rat_dst_w;
				pblend++;
			}

			dy += rat_h;
			dst += dst_p;
		}

		dy -= rat_dst_h;
		src += pitch;
	}
}

scaler_t GFX_getAAScaler(GFX_Renderer *renderer)
{
	int gcd_w, div_w, gcd_h, div_h;
	blend_args.blend_line = (uint16_t *)calloc(renderer->src_w, sizeof(uint16_t));

	gcd_w = gcd(renderer->src_w, renderer->dst_w);
	blend_args.w_ratio_in = renderer->src_w / gcd_w;
	blend_args.w_ratio_out = renderer->dst_w / gcd_w;

	double blend_denominator = (renderer->src_w > renderer->dst_w) ? 5 : 2.5; // TODO: these values are really only good for the nano...
	// blend_denominator = 5.0; // better for trimui
	// LOG_info("blend_denominator: %f (%i && %i)\n", blend_denominator, HAS_SKINNY_SCREEN, renderer->dst_w>renderer->src_w);

	div_w = round(blend_args.w_ratio_out / blend_denominator);
	blend_args.w_bp[0] = div_w;
	blend_args.w_bp[1] = blend_args.w_ratio_out >> 1;

	gcd_h = gcd(renderer->src_h, renderer->dst_h);
	blend_args.h_ratio_in = renderer->src_h / gcd_h;
	blend_args.h_ratio_out = renderer->dst_h / gcd_h;

	div_h = round(blend_args.h_ratio_out / blend_denominator);
	blend_args.h_bp[0] = div_h;
	blend_args.h_bp[1] = blend_args.h_ratio_out >> 1;

	return scaleAA;
}
void GFX_freeAAScaler(void)
{
	if (blend_args.blend_line != NULL)
	{
		free(blend_args.blend_line);
		blend_args.blend_line = NULL;
	}
}

///////////////////////////////

SDL_Color /*GFX_*/ uintToColour(uint32_t colour)
{
	SDL_Color tempcol;
	tempcol.a = 255;
	tempcol.r = (colour >> 16) & 0xFF;
	tempcol.g = (colour >> 8) & 0xFF;
	tempcol.b = colour & 0xFF;
	return tempcol;
}

SDL_Rect GFX_blitScaled(int scale, SDL_Surface *src, SDL_Surface *dst)
{
	switch (scale)
	{
	case GFX_SCALE_FIT:
		return GFX_blitScaleAspect(src, dst);
		break;
	case GFX_SCALE_FILL:
		return GFX_blitScaleToFill(src, dst);
		break;
	case GFX_SCALE_FULLSCREEN:
	default:
		return GFX_blitStretch(src, dst);
	}
}

SDL_Rect GFX_blitStretch(SDL_Surface *src, SDL_Surface *dst)
{
	if (!src || !dst)
	{
		SDL_Rect none = {0, 0};
		return none;
	}

	SDL_Rect image_rect = {0, 0, dst->w, dst->h};
	SDL_BlitScaled(src, NULL, dst, &image_rect);
	return image_rect;
}

static inline SDL_Rect GFX_scaledRectAspect(SDL_Rect src, SDL_Rect dst)
{
	SDL_Rect scaled_rect;

	// Calculate the aspect ratios
	float image_aspect = (float)src.w / (float)src.h;
	float preview_aspect = (float)dst.w / (float)dst.h;

	// Determine scaling factor
	if (image_aspect > preview_aspect)
	{
		// Image is wider than the preview area
		scaled_rect.w = dst.w;
		scaled_rect.h = (int)(dst.w / image_aspect);
	}
	else
	{
		// Image is taller than or equal to the preview area
		scaled_rect.h = dst.h;
		scaled_rect.w = (int)(dst.h * image_aspect);
	}

	// Center the scaled rectangle within preview_rect
	scaled_rect.x = dst.x + (dst.w - scaled_rect.w) / 2;
	scaled_rect.y = dst.y + (dst.h - scaled_rect.h) / 2;

	return scaled_rect;
}

SDL_Rect GFX_blitScaleAspect(SDL_Surface *src, SDL_Surface *dst)
{
	if (!src || !dst)
	{
		SDL_Rect none = {0, 0};
		return none;
	}

	SDL_Rect src_rect = {0, 0, src->w, src->h};
	SDL_Rect dst_rect = {0, 0, dst->w, dst->h};
	SDL_Rect scaled_rect = GFX_scaledRectAspect(src_rect, dst_rect);
	SDL_FillRect(dst, NULL, 0);
	SDL_BlitScaled(src, NULL, dst, &scaled_rect);
	return scaled_rect;
}

static inline SDL_Rect GFX_scaledRectAspectFill(SDL_Rect src, SDL_Rect dst)
{
	SDL_Rect scaled_rect;

	// Calculate the aspect ratios
	float image_aspect = (float)src.w / (float)src.h;
	float preview_aspect = (float)dst.w / (float)dst.h;

	// Determine scaling factor
	if (preview_aspect > image_aspect)
	{
		scaled_rect.w = src.w;
		scaled_rect.h = (int)(src.w / preview_aspect + 0.5f);
	}
	else
	{
		scaled_rect.w = (int)(src.h * preview_aspect + 0.5f);
		scaled_rect.h = src.h;
	}

	// Calculate the coordinates of the visible part of the input image
	int offsetX = abs(scaled_rect.w - src.w) / 2;
	int offsetY = abs(scaled_rect.h - src.h) / 2;

	scaled_rect.x = offsetX;
	scaled_rect.y = offsetY;

	return scaled_rect;
}

SDL_Rect GFX_blitScaleToFill(SDL_Surface *src, SDL_Surface *dst)
{
	if (!src || !dst)
	{
		SDL_Rect none = {0, 0};
		return none;
	}

	SDL_Rect src_rect = {0, 0, src->w, src->h};
	SDL_Rect dst_rect = {0, 0, dst->w, dst->h};
	SDL_Rect scaled_rect = GFX_scaledRectAspectFill(src_rect, dst_rect);
	SDL_BlitScaled(src, &scaled_rect, dst, NULL);

	return dst_rect;
}

///////////////////////////////
void GFX_ApplyRoundedCorners16(SDL_Surface *surface, SDL_Rect *rect, int radius)
{
	if (!surface || radius == 0)
		return;

	SDL_PixelFormat *fmt = surface->format;
	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

	if (fmt->format != SDL_PIXELFORMAT_RGBA8888)
	{
		SDL_Log("Unsupported pixel format: %s", SDL_GetPixelFormatName(fmt->format));
		return;
	}

	Uint16 *pixels = (Uint16 *)surface->pixels; // RGB565 uses 16-bit pixels
	Uint16 transparent_black = 0x0000;			// RGB565 has no alpha, so use black (0)

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;
	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x)
		{
			int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1)
																					: 0;
			int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1)
																					: 0;
			if (dx * dx + dy * dy > radius * radius)
			{
				pixels[y * (surface->pitch / 2) + x] = transparent_black; // Set to black (0)
			}
		}
	}
}

void GFX_ApplyRoundedCorners(SDL_Surface *surface, SDL_Rect *rect, int radius)
{
	if (!surface)
		return;

	Uint32 *pixels = (Uint32 *)surface->pixels;
	SDL_PixelFormat *fmt = surface->format;
	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

	Uint32 transparent_black = SDL_MapRGBA(fmt, 0, 0, 0, 0); // Fully transparent black

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;
	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x)
		{
			int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1)
																					: 0;
			int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1)
																					: 0;
			if (dx * dx + dy * dy > radius * radius)
			{
				pixels[y * target.w + x] = transparent_black; // Set to fully transparent black
			}
		}
	}
}

void GFX_ApplyRoundedCorners_4444(SDL_Surface *surface, SDL_Rect *rect, int radius)
{
	if (!surface || 
		(surface->format->format != SDL_PIXELFORMAT_RGBA4444 && surface->format->format != SDL_PIXELFORMAT_ARGB4444))
		return;

	Uint16 *pixels = (Uint16 *)surface->pixels;
	SDL_PixelFormat *fmt = surface->format;
	int pitch = surface->pitch / 2;
	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

	Uint16 transparent_black = SDL_MapRGB(fmt, 0, 0, 0); // Fully transparent black

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;
	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x)
		{
			int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1)
																					: 0;
			int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1)
																					: 0;
			if (dx * dx + dy * dy > radius * radius)
			{
				pixels[y * pitch + x] = transparent_black;
			}
		}
	}
}

void GFX_ApplyRoundedCorners_8888(SDL_Surface *surface, SDL_Rect *rect, int radius)
{
	if (!surface || 
		(surface->format->format != SDL_PIXELFORMAT_RGBA8888 && surface->format->format != SDL_PIXELFORMAT_ARGB8888))
		return;

	Uint32 *pixels = (Uint32 *)surface->pixels;
	SDL_PixelFormat *fmt = surface->format;
	int pitch = surface->pitch / 4; // Since each pixel is 4 bytes in RGBA8888

	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

	Uint32 transparent_black = SDL_MapRGBA(fmt, 0, 0, 0, 0); // Fully transparent black

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;

	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x)
		{
			int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1)
																					: 0;
			int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1)
																					: 0;

			// Check if the pixel is outside the rounded corner radius
			if (dx * dx + dy * dy > radius * radius)
			{
				pixels[y * pitch + x] = transparent_black;
			}
		}
	}
}

void GFX_blitSurfaceColor(SDL_Surface *src, SDL_Rect *src_rect, SDL_Surface *dst, SDL_Rect *dst_rect, uint32_t asset_color)
{
	// This could be a RAII
	if (asset_color != RGB_WHITE)
	{
		// TODO: Is there a neat way to get the opposite effect of SDL_MapRGB?
		// This is kinda ugly and not very generic.
		if (asset_color == THEME_COLOR1)
			asset_color = THEME_COLOR1_255;
		else if (asset_color == THEME_COLOR2)
			asset_color = THEME_COLOR2_255;
		else if (asset_color == THEME_COLOR3)
			asset_color = THEME_COLOR3_255;
		else if (asset_color == THEME_COLOR4)
			asset_color = THEME_COLOR4_255;
		else if (asset_color == THEME_COLOR5)
			asset_color = THEME_COLOR5_255;
		else if (asset_color == THEME_COLOR6)
			asset_color = THEME_COLOR6_255;
		else if (asset_color == THEME_COLOR7)
			asset_color = THEME_COLOR7_255;

		SDL_Color restore;
		SDL_GetSurfaceColorMod(src, &restore.r, &restore.g, &restore.b);
		SDL_SetSurfaceColorMod(src,
							   (asset_color >> 16) & 0xFF,
							   (asset_color >> 8) & 0xFF,
							   asset_color & 0xFF);
		SDL_BlitSurface(src, src_rect, dst, dst_rect);
		SDL_SetSurfaceColorMod(src, restore.r, restore.g, restore.b);
	}
	else
	{
		SDL_BlitSurface(src, src_rect, dst, dst_rect);
	}
}

void GFX_blitAssetColor(int asset, SDL_Rect *src_rect, SDL_Surface *dst, SDL_Rect *dst_rect, uint32_t asset_color)
{

	SDL_Rect *rect = &asset_rects[asset];
	SDL_Rect adj_rect = {
		.x = rect->x,
		.y = rect->y,
		.w = rect->w,
		.h = rect->h,
	};
	if (src_rect)
	{
		adj_rect.x += src_rect->x;
		adj_rect.y += src_rect->y;
		adj_rect.w = src_rect->w;
		adj_rect.h = src_rect->h;
	}

	GFX_blitSurfaceColor(gfx.assets, &adj_rect, dst, dst_rect, asset_color);
}
void GFX_blitAsset(int asset, SDL_Rect *src_rect, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	GFX_blitAssetColor(asset, src_rect, dst, dst_rect, RGB_WHITE);
}
void GFX_blitPillColor(int asset, SDL_Surface *dst, SDL_Rect *dst_rect, uint32_t asset_color, uint32_t fill_color)
{
	int x = dst_rect->x;
	int y = dst_rect->y;
	int w = dst_rect->w;
	int h = dst_rect->h;

	if (h == 0)
		h = asset_rects[asset].h;

	int r = h / 2;
	if (w < h)
		w = h;
	w -= h;

	GFX_blitAssetColor(asset, &(SDL_Rect){0, 0, r, h}, dst, &(SDL_Rect){x, y}, asset_color);
	x += r;
	if (w > 0)
	{
		// SDL_FillRect(dst, &(SDL_Rect){x,y,w,h}, UintMult(fill_color, asset_color));
		SDL_FillRect(dst, &(SDL_Rect){x, y, w, h}, asset_color);
		x += w;
	}
	GFX_blitAssetColor(asset, &(SDL_Rect){r, 0, r, h}, dst, &(SDL_Rect){x, y}, asset_color);
}
void GFX_blitPill(int asset, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	GFX_blitPillColor(asset, dst, dst_rect, asset_rgbs[asset], RGB_WHITE);
}
void GFX_blitPillLight(int asset, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	GFX_blitPillColor(asset, dst, dst_rect, THEME_COLOR2, RGB_WHITE);
}
void GFX_blitPillDark(int asset, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	GFX_blitPillColor(asset, dst, dst_rect, THEME_COLOR1, RGB_WHITE);
}
void GFX_blitRect(int asset, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	int c = asset_rgbs[asset];
	GFX_blitRectColor(asset, dst, dst_rect, c);
}
void GFX_blitRectColor(int asset, SDL_Surface *dst, SDL_Rect *dst_rect, uint32_t asset_color)
{
	int x = dst_rect->x;
	int y = dst_rect->y;
	int w = dst_rect->w;
	int h = dst_rect->h;

	SDL_Rect *rect = &asset_rects[asset];
	int d = rect->w;
	int r = d / 2;

	GFX_blitAssetColor(asset, &(SDL_Rect){0, 0, r, r}, dst, &(SDL_Rect){x, y}, asset_color);
	SDL_FillRect(dst, &(SDL_Rect){x + r, y, w - d, r}, asset_color);
	GFX_blitAssetColor(asset, &(SDL_Rect){r, 0, r, r}, dst, &(SDL_Rect){x + w - r, y}, asset_color);
	SDL_FillRect(dst, &(SDL_Rect){x, y + r, w, h - d}, asset_color);
	GFX_blitAssetColor(asset, &(SDL_Rect){0, r, r, r}, dst, &(SDL_Rect){x, y + h - r}, asset_color);
	SDL_FillRect(dst, &(SDL_Rect){x + r, y + h - r, w - d, r}, asset_color);
	GFX_blitAssetColor(asset, &(SDL_Rect){r, r, r, r}, dst, &(SDL_Rect){x + w - r, y + h - r}, asset_color);
}

void GFX_assetRect(int asset, SDL_Rect *dst_rect)
{
	*dst_rect = asset_rects[asset];
}

void GFX_blitBattery(SDL_Surface *dst, SDL_Rect *dst_rect)
{
	int x = 0;
	int y = 0;
	if (dst_rect)
	{
		x = dst_rect->x;
		y = dst_rect->y;
	}
	GFX_blitBatteryAtPosition(dst, x, y);
}

int GFX_getButtonWidth(char *hint, char *button)
{
	int button_width = 0;
	int width;

	int special_case = !strcmp(button, BRIGHTNESS_BUTTON_LABEL); // TODO: oof

	if (strlen(button) == 1)
	{
		button_width += SCALE1(BUTTON_SIZE);
	}
	else
	{
		button_width += SCALE1(BUTTON_SIZE) / 2;
		TTF_SizeUTF8(special_case ? font.large : font.tiny, button, &width, NULL);
		button_width += width;
	}
	button_width += SCALE1(BUTTON_MARGIN);

	TTF_SizeUTF8(font.small, hint, &width, NULL);
	button_width += width + SCALE1(BUTTON_MARGIN);
	return button_width;
}
void GFX_blitButton(char *hint, char *button, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	SDL_Surface *text;
	int ox = 0;

	int special_case = !strcmp(button, BRIGHTNESS_BUTTON_LABEL); // TODO: oof

	// button
	if (strlen(button) == 1)
	{
		GFX_blitAssetColor(ASSET_BUTTON, NULL, dst, dst_rect, THEME_COLOR1);

		// label
		text = TTF_RenderUTF8_Blended(font.medium, button, ALT_BUTTON_TEXT_COLOR);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){dst_rect->x + (SCALE1(BUTTON_SIZE) - text->w) / 2, dst_rect->y + (SCALE1(BUTTON_SIZE) - text->h) / 2});
		ox += SCALE1(BUTTON_SIZE);
		SDL_FreeSurface(text);
	}
	else
	{
		text = TTF_RenderUTF8_Blended(special_case ? font.large : font.tiny, button, ALT_BUTTON_TEXT_COLOR);
		GFX_blitPillDark(ASSET_BUTTON, dst, &(SDL_Rect){dst_rect->x, dst_rect->y, SCALE1(BUTTON_SIZE) / 2 + text->w, SCALE1(BUTTON_SIZE)});
		ox += SCALE1(BUTTON_SIZE) / 4;

		int oy = special_case ? SCALE1(-2) : 0;
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){ox + dst_rect->x, oy + dst_rect->y + (SCALE1(BUTTON_SIZE) - text->h) / 2, text->w, text->h});
		ox += text->w;
		ox += SCALE1(BUTTON_SIZE) / 4;
		SDL_FreeSurface(text);
	}

	ox += SCALE1(BUTTON_MARGIN);

	// hint text
	SDL_Color text_color = uintToColour(THEME_COLOR6_255);
	text = TTF_RenderUTF8_Blended(font.small, hint, text_color);
	SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){ox + dst_rect->x, dst_rect->y + (SCALE1(BUTTON_SIZE) - text->h) / 2, text->w, text->h});
	SDL_FreeSurface(text);
}
void GFX_blitMessage(TTF_Font *font, char *msg, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	if (!dst_rect)
		dst_rect = &(SDL_Rect){0, 0, dst->w, dst->h};

	// LOG_info("GFX_blitMessage: %p (%ix%i)", dst, dst_rect->w,dst_rect->h);

	SDL_Surface *text;
#define TEXT_BOX_MAX_ROWS 16
	char *rows[TEXT_BOX_MAX_ROWS];
	int row_count = 0;

	char *tmp;
	rows[row_count++] = msg;
	while ((tmp = strchr(rows[row_count - 1], '\n')) != NULL)
	{
		if (row_count + 1 >= TEXT_BOX_MAX_ROWS)
			return; // TODO: bail
		rows[row_count++] = tmp + 1;
	}

	int line_height = TTF_FontHeight(font);
	int rendered_height = line_height * row_count;
	int y = dst_rect->y;
	y += (dst_rect->h - rendered_height) / 2;

	char line[256];
	for (int i = 0; i < row_count; i++)
	{
		int len;
		if (i + 1 < row_count)
		{
			len = rows[i + 1] - rows[i] - 1;
			if (len)
				strncpy(line, rows[i], len);
			line[len] = '\0';
		}
		else
		{
			len = strlen(rows[i]);
			strcpy(line, rows[i]);
		}

		if (len)
		{
			text = TTF_RenderUTF8_Blended_Wrapped(font, line, COLOR_WHITE, dst_rect->w);
			int x = dst_rect->x;
			x += (dst_rect->w - text->w) / 2;
			SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){x, y});
			SDL_FreeSurface(text);
		}
		y += line_height;
	}
}

void GFX_blitBatteryAtPosition(SDL_Surface *dst, int x, int y)
{
	SDL_Rect battery_rect = asset_rects[ASSET_BATTERY];

	if (SDL_AtomicGet(&pwr.is_charging))
	{
		GFX_blitAssetColor(ASSET_BATTERY, NULL, dst, &(SDL_Rect){x, y}, THEME_COLOR6);
		GFX_blitAssetColor(ASSET_BATTERY_BOLT, NULL, dst, &(SDL_Rect){x + SCALE1(3), y + SCALE1(2)}, THEME_COLOR6);
	}
	else
	{
		int percent = SDL_AtomicGet(&pwr.charge);
		GFX_blitAssetColor(percent <= 10 ? ASSET_BATTERY_LOW : ASSET_BATTERY, NULL, dst, &(SDL_Rect){x, y}, THEME_COLOR6);

		if (CFG_getShowBatteryPercent())
		{
			char percentage[16];
			sprintf(percentage, "%i", SDL_AtomicGet(&pwr.charge));
			SDL_Surface *text = TTF_RenderUTF8_Blended(font.micro, percentage, uintToColour(THEME_COLOR6_255));
			SDL_Rect target = {
				x + (battery_rect.w - text->w) / 2 + 1,
				y + (battery_rect.h - text->h) / 2 - 1};
			SDL_BlitSurface(text, NULL, dst, &target);
			SDL_FreeSurface(text);
		}
		else
		{
			SDL_Rect fill_rect = asset_rects[ASSET_BATTERY_FILL];
			SDL_Rect clip = fill_rect;
			clip.w *= percent;
			clip.w /= 100;
			if (clip.w > 0)
			{
				clip.x = fill_rect.w - clip.w;
				clip.y = 0;
				GFX_blitAssetColor(percent <= 20 ? ASSET_BATTERY_FILL_LOW : ASSET_BATTERY_FILL, &clip, dst,
								   &(SDL_Rect){x + SCALE1(3) + clip.x, y + SCALE1(2)}, THEME_COLOR6);
			}
		}
	}
}

// Helper function to render a hardware indicator (volume/brightness/colortemp) at a specific position.
// This is the reusable core extracted from GFX_blitHardwareGroup for use in notifications.
int GFX_blitHardwareIndicator(SDL_Surface *dst, int x, int y, IndicatorType indicator_type)
{
	int setting_value;
	int setting_min;
	int setting_max;
	int asset;
	
	int ow = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
	int ox = x;
	int oy = y;
	
	// Draw the pill background
	GFX_blitPillLight(ASSET_WHITE_PILL, dst, &(SDL_Rect){ox, oy, ow, SCALE1(PILL_SIZE)});
	
	// Determine which setting to display
	if (indicator_type == INDICATOR_BRIGHTNESS)
	{
		setting_value = GetBrightness();
		setting_min = BRIGHTNESS_MIN;
		setting_max = BRIGHTNESS_MAX;
		asset = ASSET_BRIGHTNESS;
	}
	else if (indicator_type == INDICATOR_COLORTEMP)
	{
		setting_value = GetColortemp();
		setting_min = COLORTEMP_MIN;
		setting_max = COLORTEMP_MAX;
		asset = ASSET_COLORTEMP;
	}
	else // INDICATOR_VOLUME
	{
		setting_value = GetVolume();
		setting_min = VOLUME_MIN;
		setting_max = VOLUME_MAX;
		if(GetAudioSink() == AUDIO_SINK_BLUETOOTH)
			asset = (setting_value > 0 ? ASSET_BLUETOOTH : ASSET_BLUETOOTH_OFF);
		else
			asset = (setting_value > 0 ? ASSET_VOLUME : ASSET_VOLUME_MUTE);
	}
	
	// Draw the icon
	SDL_Rect asset_rect;
	GFX_assetRect(asset, &asset_rect);
	int ax = ox + (SCALE1(PILL_SIZE) - asset_rect.w) / 2;
	int ay = oy + (SCALE1(PILL_SIZE) - asset_rect.h) / 2;
	GFX_blitAssetColor(asset, NULL, dst, &(SDL_Rect){ax, ay}, THEME_COLOR6_255);
	
	// Draw the progress bar background
	ox += SCALE1(PILL_SIZE);
	int bar_y = y + SCALE1((PILL_SIZE - SETTINGS_SIZE) / 2);
	GFX_blitPillColor(gfx.mode == MODE_MAIN ? ASSET_BAR_BG : ASSET_BAR_BG_MENU, dst, 
		&(SDL_Rect){ox, bar_y, SCALE1(SETTINGS_WIDTH), SCALE1(SETTINGS_SIZE)}, THEME_COLOR3, RGB_WHITE);
	
	// Draw the progress bar fill
	float percent = ((float)(setting_value - setting_min) / (setting_max - setting_min));
	if (indicator_type == 1 || indicator_type == 3 || setting_value > 0)
	{
		GFX_blitPillDark(ASSET_BAR, dst, &(SDL_Rect){ox, bar_y, SCALE1(SETTINGS_WIDTH) * percent, SCALE1(SETTINGS_SIZE)});
	}
	
	return ow;
}

SDL_Surface* GFX_createScreenFormatSurface(int width, int height)
{
	if (!gfx.screen) return NULL;
	return SDL_CreateRGBSurfaceWithFormat(
		SDL_SWSURFACE, width, height, 
		gfx.screen->format->BitsPerPixel, 
		gfx.screen->format->format
	);
}

int GFX_blitHardwareGroup(SDL_Surface *dst, int show_setting)
{
	int ox;
	int oy;
	int ow = 0;

	if (show_setting && !GetHDMI())
	{
		// Use the helper function to render the indicator at the standard position
		ow = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
		ox = dst->w - SCALE1(PADDING) - ow;
		oy = SCALE1(PADDING);
		GFX_blitHardwareIndicator(dst, ox, oy, (IndicatorType)show_setting);
	}
	else
	{
		ConnectionStrength strength = PLAT_connectionStrength();
		int show_wifi = strength > SIGNAL_STRENGTH_OFF;
		// no need to handle in PLAT_updateNetworkStatus,
		// this one is async anyway
		int show_bt = BT_isConnected();
		bool show_clock = CFG_getShowClock();
		SDL_Rect battery_rect = asset_rects[ASSET_BATTERY];

		if (!show_bt && !show_wifi && !show_clock)
		{
			ow = SCALE1(PILL_SIZE);
			ox = dst->w - SCALE1(PADDING) - ow;
			oy = SCALE1(PADDING);

			GFX_blitPillColor(ASSET_WHITE_PILL, dst, &(SDL_Rect){ox, oy, ow, SCALE1(PILL_SIZE)}, THEME_COLOR2, RGB_WHITE);

			int battery_x = ox + (SCALE1(PILL_SIZE) - (battery_rect.w + FIXED_SCALE)) / 2;
			int battery_y = oy + (SCALE1(PILL_SIZE) - battery_rect.h) / 2;

			GFX_blitBatteryAtPosition(dst, battery_x, battery_y);
		}
		else
		{
			ow = SCALE1(BUTTON_MARGIN);

			if (show_bt)
			{
				SDL_Rect bt_rect = asset_rects[ASSET_BLUETOOTH];
				ow += bt_rect.w + SCALE1(BUTTON_MARGIN);
			}

			if (show_wifi)
			{
				SDL_Rect wifi_rect = asset_rects[ASSET_WIFI];
				ow += wifi_rect.w + SCALE1(BUTTON_MARGIN);
			}

			ow += battery_rect.w + SCALE1(BUTTON_MARGIN);

			SDL_Surface *clock = NULL;
			if (show_clock)
			{
				int clock_width = 0;
				char timeString[12];
				time_t t = time(NULL);
				struct tm tm = *localtime(&t);
				if (CFG_getClock24H())
					strftime(timeString, 12, "%R", &tm);
				else
					strftime(timeString, 12, "%-I:%M %p", &tm);
				char display_name[12];
				clock_width = GFX_getTextWidth(font.small, timeString, display_name, SCALE1(PILL_SIZE), 0);
				clock = TTF_RenderUTF8_Blended(font.small, display_name, uintToColour(THEME_COLOR6_255));
				ow += clock_width + SCALE1(BUTTON_MARGIN);
			}

			ox = dst->w - SCALE1(PADDING) - ow;
			oy = SCALE1(PADDING);
			GFX_blitPillColor(ASSET_WHITE_PILL, dst, &(SDL_Rect){ox, oy, ow, SCALE1(PILL_SIZE)}, THEME_COLOR2, RGB_WHITE);

			ox += SCALE1(BUTTON_MARGIN);

			if (show_bt)
			{
				int asset = ASSET_BLUETOOTH;
				SDL_Rect bt_rect = asset_rects[asset];
				int x = ox;
				int y = oy + (SCALE1(PILL_SIZE) - bt_rect.h) / 2;

				GFX_blitAssetColor(asset, NULL, dst, &(SDL_Rect){x, y}, THEME_COLOR6);
				ox += bt_rect.w + SCALE1(BUTTON_MARGIN);
			}

			if (show_wifi)
			{
				int asset =
					strength == SIGNAL_STRENGTH_HIGH ? ASSET_WIFI : strength == SIGNAL_STRENGTH_MED ? ASSET_WIFI_MED
																: strength == SIGNAL_STRENGTH_LOW	? ASSET_WIFI_LOW
																									: ASSET_WIFI_OFF; // this should use ASSET_WIFI and be greyed out
				SDL_Rect wifi_rect = asset_rects[asset];
				int x = ox;
				int y = oy + (SCALE1(PILL_SIZE) - wifi_rect.h) / 2;

				GFX_blitAssetColor(asset, NULL, dst, &(SDL_Rect){x, y}, THEME_COLOR6);
				ox += wifi_rect.w + SCALE1(BUTTON_MARGIN);
			}

			int battery_x = ox;
			int battery_y = oy + (SCALE1(PILL_SIZE) - battery_rect.h) / 2;

			GFX_blitBatteryAtPosition(dst, battery_x, battery_y);
			ox += battery_rect.w + SCALE1(BUTTON_MARGIN);

			if (show_clock && clock)
			{
				int x = ox;
				int y = oy + (SCALE1(PILL_SIZE) - clock->h) / 2;
				SDL_BlitSurface(clock, NULL, dst, &(SDL_Rect){x, y});
				SDL_FreeSurface(clock);
			}
		}
	}

	return ow;
}
void GFX_blitHardwareHints(SDL_Surface *dst, int show_setting)
{

	if (show_setting == 1)
		GFX_blitButtonGroup((char *[]){BRIGHTNESS_BUTTON_LABEL, "BRIGHTNESS", NULL}, 0, dst, 0);
	else if (show_setting == 3)
		GFX_blitButtonGroup((char *[]){BRIGHTNESS_BUTTON_LABEL, "COLOR TEMP", NULL}, 0, dst, 0);
	else
		GFX_blitButtonGroup((char *[]){"MNU", "BRGHT", "SEL", "TEMP", NULL}, 0, dst, 0);
}

int GFX_blitButtonGroup(char **pairs, int primary, SDL_Surface *dst, int align_right)
{
	int ox;
	int oy;
	int ow;
	char *hint;
	char *button;

	struct Hint
	{
		char *hint;
		char *button;
		int ow;
	} hints[2];
	int w = 0; // individual button dimension
	int h = 0; // hints index
	ow = 0;	   // full pill width
	ox = align_right ? dst->w - SCALE1(PADDING) : SCALE1(PADDING);
	oy = dst->h - SCALE1(PADDING + PILL_SIZE);

	for (int i = 0; i < 2; i++)
	{
		if (!pairs[i * 2])
			break;
		if (HAS_SKINNY_SCREEN && i != primary)
			continue; // space saving

		button = pairs[i * 2];
		hint = pairs[i * 2 + 1];
		w = GFX_getButtonWidth(hint, button);
		hints[h].hint = hint;
		hints[h].button = button;
		hints[h].ow = w;
		h += 1;
		ow += SCALE1(BUTTON_MARGIN) + w;
	}

	ow += SCALE1(BUTTON_MARGIN);
	if (align_right)
		ox -= ow;
	GFX_blitPillColor(ASSET_WHITE_PILL, dst, &(SDL_Rect){ox, oy, ow, SCALE1(PILL_SIZE)}, THEME_COLOR2, RGB_WHITE);

	ox += SCALE1(BUTTON_MARGIN);
	oy += SCALE1(BUTTON_MARGIN);
	for (int i = 0; i < h; i++)
	{
		GFX_blitButton(hints[i].hint, hints[i].button, dst, &(SDL_Rect){ox, oy});
		ox += hints[i].ow + SCALE1(BUTTON_MARGIN);
	}
	return ow;
}

#define MAX_TEXT_LINES 16
void GFX_sizeText(TTF_Font *font, const char *str, int leading, int *w, int *h)
{
	const char *lines[MAX_TEXT_LINES];
	int count = 0;

	const char *tmp;
	lines[count++] = str;
	while ((tmp = strchr(lines[count - 1], '\n')) != NULL)
	{
		if (count + 1 > MAX_TEXT_LINES)
			break; // TODO: bail?
		lines[count++] = tmp + 1;
	}
	*h = count * leading;

	int mw = 0;
	char line[256];
	for (int i = 0; i < count; i++)
	{
		int len;
		if (i + 1 < count)
		{
			len = lines[i + 1] - lines[i] - 1;
			if (len)
				strncpy(line, lines[i], len);
			line[len] = '\0';
		}
		else
		{
			len = strlen(lines[i]);
			strcpy(line, lines[i]);
		}

		if (len)
		{
			int lw;
			TTF_SizeUTF8(font, line, &lw, NULL);
			if (lw > mw)
				mw = lw;
		}
	}
	*w = mw;
}
void GFX_blitText(TTF_Font *font, const char *str, int leading, SDL_Color color, SDL_Surface *dst, SDL_Rect *dst_rect)
{
	if (dst_rect == NULL)
		dst_rect = &(SDL_Rect){0, 0, dst->w, dst->h};

	const char *lines[MAX_TEXT_LINES];
	int count = 0;

	const char *tmp;
	lines[count++] = str;
	while ((tmp = strchr(lines[count - 1], '\n')) != NULL)
	{
		if (count + 1 > MAX_TEXT_LINES)
			break; // TODO: bail?
		lines[count++] = tmp + 1;
	}
	int x = dst_rect->x;
	int y = dst_rect->y;

	SDL_Surface *text;
	char line[256];
	for (int i = 0; i < count; i++)
	{
		int len;
		if (i + 1 < count)
		{
			len = lines[i + 1] - lines[i] - 1;
			if (len)
				strncpy(line, lines[i], len);
			line[len] = '\0';
		}
		else
		{
			len = strlen(lines[i]);
			strcpy(line, lines[i]);
		}

		if (len)
		{
			text = TTF_RenderUTF8_Blended(font, line, color);
			SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){x + ((dst_rect->w - text->w) / 2), y + (i * leading)});
			SDL_FreeSurface(text);
		}
	}
}

SDL_Color GFX_mapColor(uint32_t c)
{
	return uintToColour(c);
}

///////////////////////////////

// based on picoarch's audio
// implementation, rewritten
// to (try to) understand it
// better

#define MAX_SAMPLE_RATE 48000
#define BATCH_SIZE 100
#ifndef SAMPLES
#define SAMPLES 512 // default
#endif

#define ms SDL_GetTicks

pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;

static void SND_audioCallback(void *userdata, uint8_t *stream, int len)
{
	if (snd.frame_count == 0)
		return;
	if (!snd.initialized)
		LOG_error("Calling callback without audio device\n");

	int16_t *out = (int16_t *)stream;
	len /= (sizeof(int16_t) * 2);

	pthread_mutex_lock(&audio_mutex);
	while (snd.frame_out != snd.frame_in && len > 0)
	{
		*out++ = snd.buffer[snd.frame_out].left;
		*out++ = snd.buffer[snd.frame_out].right;
		snd.frame_out += 1;
		len -= 1;
		if (snd.frame_out >= snd.frame_count)
			snd.frame_out = 0;
	}
	pthread_mutex_unlock(&audio_mutex);

	if (len > 0)
		memset(out, 0, len * (sizeof(int16_t) * 2));
}
static void SND_resizeBuffer(void)
{ // plat_sound_resize_buffer

	LOG_info("Resizing audio buffer for new frame count: %d\n", snd.frame_count);

	if (snd.frame_count == 0)
		return;

#if defined(USE_SDL2)
	SDL_LockAudioDevice(snd.device_id);
#else
	SDL_LockAudio();
#endif

	int buffer_bytes = snd.frame_count * sizeof(SND_Frame);
	snd.buffer = (SND_Frame *)realloc(snd.buffer, buffer_bytes);

	LOG_info("Resized audio buffer to: %d bytes\n", buffer_bytes);

	memset(snd.buffer, 0, buffer_bytes);

	snd.frame_in = 0;
	snd.frame_out = 0;

#if defined(USE_SDL2)
	SDL_UnlockAudioDevice(snd.device_id);
#else
	SDL_UnlockAudio();
#endif
}
static int soundQuality = 2;
static int resetSrcState = 0;
void SND_setQuality(int quality)
{
	LOG_info("Set sound quality\n");
	soundQuality = qualityLevels[quality];
	resetSrcState = 1;
}
ResampledFrames resample_audio(const SND_Frame *input_frames,
							   int input_frame_count, int input_sample_rate,
							   int output_sample_rate, double ratio)
{

	int error;
	static double previous_ratio = 1.0;
	static SRC_STATE *src_state = NULL;

	double final_ratio = ((double)output_sample_rate / input_sample_rate) * ratio;

	if (!src_state || resetSrcState)
	{
		resetSrcState = 0;
		src_state = src_new(soundQuality, 2, &error);
		if (src_state == NULL)
		{
			fprintf(stderr, "Error initializing SRC state: %s\n",
					src_strerror(error));
			exit(1);
		}
	}

	if (previous_ratio != final_ratio)
	{
		if (src_set_ratio(src_state, final_ratio) != 0)
		{
			fprintf(stderr, "Error setting resampling ratio: %s\n",
					src_strerror(src_error(src_state)));
			exit(1);
		}
		previous_ratio = final_ratio;
	}

	int max_output_frames = (int)(input_frame_count * final_ratio + 1);

	float *input_buffer = (float *)malloc(input_frame_count * 2 * sizeof(float));
	float *output_buffer = (float *)malloc(max_output_frames * 2 * sizeof(float));
	if (!input_buffer || !output_buffer)
	{
		fprintf(stderr, "Error allocating buffers\n");
		free(input_buffer);
		free(output_buffer);
		src_delete(src_state);
		exit(1);
	}

	for (int i = 0; i < input_frame_count; i++)
	{
		input_buffer[2 * i] = input_frames[i].left / 32768.0f;
		input_buffer[2 * i + 1] = input_frames[i].right / 32768.0f;
	}

	SRC_DATA src_data = {
		.data_in = input_buffer,
		.data_out = output_buffer,
		.input_frames = input_frame_count,
		.output_frames = max_output_frames,
		.src_ratio = final_ratio,
		.end_of_input = 0};

	if (src_process(src_state, &src_data) != 0)
	{
		fprintf(stderr, "Error resampling: %s\n",
				src_strerror(src_error(src_state)));
		free(input_buffer);
		free(output_buffer);
		exit(1);
	}

	int output_frame_count = src_data.output_frames_gen;

	SND_Frame *output_frames = (SND_Frame *)malloc(output_frame_count * sizeof(SND_Frame));
	if (!output_frames)
	{
		fprintf(stderr, "Error allocating output frames\n");
		free(input_buffer);
		free(output_buffer);
		exit(1);
	}

	for (int i = 0; i < output_frame_count; i++)
	{
		float left = output_buffer[2 * i];
		float right = output_buffer[2 * i + 1];

		left = fmaxf(-1.0f, fminf(1.0f, left));
		right = fmaxf(-1.0f, fminf(1.0f, right));

		output_frames[i].left = (int16_t)(left * 32767.0f);
		output_frames[i].right = (int16_t)(right * 32767.0f);
	}

	free(input_buffer);
	free(output_buffer);

	ResampledFrames resampled;
	resampled.frames = output_frames;
	resampled.frame_count = output_frame_count;

	return resampled;
}

#define ROLLING_AVERAGE_WINDOW_SIZE 120
static float adjustment_history[ROLLING_AVERAGE_WINDOW_SIZE] = {0.0f};
static int adjustment_index = 0;
static float remaining_space_history[ROLLING_AVERAGE_WINDOW_SIZE] = {0.0f};
static int remaining_space_index = 0;

float calculateBufferAdjustment(float remaining_space, float targetbuffer_over, float targetbuffer_under, int batchsize)
{

	// this is just to show average remaining space in debug window could be removed later
	remaining_space_history[remaining_space_index] = remaining_space;
	remaining_space_index = (remaining_space_index + 1) % ROLLING_AVERAGE_WINDOW_SIZE;
	float avgspace = 0.0f;
	for (int i = 0; i < ROLLING_AVERAGE_WINDOW_SIZE; ++i)
	{
		avgspace += remaining_space_history[i];
	}
	avgspace /= ROLLING_AVERAGE_WINDOW_SIZE;
	perf.avg_buffer_free = avgspace;
	// end debug part

	float midpoint = (targetbuffer_over + targetbuffer_under) / 2.0f;
	perf.buffer_target = midpoint;

	float normalizedDistance;
	if (remaining_space < midpoint)
	{
		normalizedDistance = (midpoint - remaining_space) / (midpoint - targetbuffer_over);
	}
	else
	{
		normalizedDistance = (remaining_space - midpoint) / (targetbuffer_under - midpoint);
	}
	// I make crazy small adjustments, mooore tiny is mooore stable :D But don't come neir the limits cuz imma hit ya with that 0.005 ratio adjustment, pow pow!
	// I wonder if staying in the middle of 0 to 4000 with 512 samples per batch playing at tiny different speeds each iteration is like the smallest I can get
	// lets say hovering around 2000 means 2000 samples queue, about 4 frames, so at 17ms(60fps) thats  68ms delay right?
	// Should have payed attention when my math teacher was talking dammit
	// Also I chose 3 for pow, but idk if that really the best nr, anyone good in maths looking at my code?
	float adjustment = 0.001f + (0.01f - 0.001f) * pow(normalizedDistance, 3);

	if (remaining_space < midpoint)
	{
		adjustment = -adjustment;
	}

	adjustment_history[adjustment_index] = adjustment;
	adjustment_index = (adjustment_index + 1) % ROLLING_AVERAGE_WINDOW_SIZE;

	// Calculate the rolling average
	float rolling_average = 0.0f;
	for (int i = 0; i < ROLLING_AVERAGE_WINDOW_SIZE; ++i)
	{
		rolling_average += adjustment_history[i];
	}
	rolling_average /= ROLLING_AVERAGE_WINDOW_SIZE;
	return rolling_average;
}

static SND_Frame tmpbuffer[BATCH_SIZE];
static SND_Frame *unwritten_frames = NULL;
static int unwritten_frame_count = 0;

size_t SND_batchSamples(const SND_Frame *frames, size_t frame_count)
{
	int framecount = (int)frame_count;
	int consumed = 0;
	int total_consumed_frames = 0;
	double ratio = 1.0;

	if (snd.frame_count <= 0)
	{
		snd.frame_count = 4096; // idk some random samples nr this should never hit tho, just to be safe
	}

	pthread_mutex_lock(&audio_mutex);
	if (snd.frame_in < 0 || snd.frame_in >= snd.frame_count)
	{
		snd.frame_in = 0;
	}

	if (snd.frame_out < 0 || snd.frame_out >= snd.frame_count)
	{
		snd.frame_out = 0;
	}

	float remaining_space = 0.0f;
	int frame_in_snapshot = snd.frame_in;
	int frame_out_snapshot = snd.frame_out;
	pthread_mutex_unlock(&audio_mutex);
	
	if (frame_in_snapshot >= frame_out_snapshot)
	{
		remaining_space = snd.frame_count - (frame_in_snapshot - frame_out_snapshot);
	}
	else
	{
		remaining_space = frame_out_snapshot - frame_in_snapshot;
	}
	perf.buffer_free = remaining_space;

	// let audio buffer fill a little first and then unpause audio so no underruns occur
	if (perf.buffer_free < snd.frame_count * 0.6f) {
		SND_pauseAudio(false);
	} else if (perf.buffer_free > snd.frame_count * 0.99f) { // if for some reason buffer drops below threshold again, pause it (like psx core can stop sending audio in between scenes or after fast forward etc)
		SND_pauseAudio(true);
	}


	float tempdelay = ((snd.frame_count - remaining_space) / snd.sample_rate_out) * 1000.0f;
	perf.buffer_ms = tempdelay;

	// do some checks
	if (current_fps <= 0.0f || !isfinite(current_fps))
	{
		current_fps = 0.01f;
	}
	if (!isfinite(snd.frame_rate) || snd.frame_rate <= 0.0f)
	{
		snd.frame_rate = 60.0f;
	}

	float bufferadjustment = calculateBufferAdjustment(remaining_space, snd.frame_count*0.2, snd.frame_count*0.8, frame_count);

	if (!isfinite(bufferadjustment))
	{
		bufferadjustment = 0.0f;
	}

	float safe_ratio = snd.frame_rate / current_fps;
	if (!isfinite(safe_ratio))
	{
		safe_ratio = 1.0f;
	}

	ratio = safe_ratio + bufferadjustment;

	if (!isfinite(ratio))
	{
		ratio = 1.0;
	}

	// limit ratio so it wont go crazy for some reason
	if (ratio > 1.5)
		ratio = 1.5;
	else if (ratio < 0.5)
		ratio = 0.5;

	perf.ratio = (ratio > 0.0) ? ratio : current_fps;

	while (framecount > 0)
	{
		int amount = MIN(BATCH_SIZE, framecount);

		// Copy frames to tmpbuffer for resampling
		for (int i = 0; i < amount; i++)
		{
			tmpbuffer[i] = frames[consumed + i];
		}
		consumed += amount;
		framecount -= amount;

		ResampledFrames resampled = resample_audio(
			tmpbuffer, amount, snd.sample_rate_in, snd.sample_rate_out, ratio);

		int written_frames = 0;
		pthread_mutex_lock(&audio_mutex);
		for (int i = 0; i < resampled.frame_count; i++)
		{
			// Check if buffer full (leave one slot free)
			if ((snd.frame_in + 1) % snd.frame_count == snd.frame_out)
			{
				// Buffer full, break early
				break;
			}
			snd.buffer[snd.frame_in] = resampled.frames[i];
			snd.frame_in = (snd.frame_in + 1) % snd.frame_count;
			written_frames++;
		}
		pthread_mutex_unlock(&audio_mutex);

		total_consumed_frames += written_frames;
		free(resampled.frames);
	}

	return total_consumed_frames;
}

enum
{
	SND_FF_ON_TIME,
	SND_FF_LATE,
	SND_FF_VERY_LATE
};

size_t SND_batchSamples_fixed_rate(const SND_Frame *frames, size_t frame_count)
{
	static int current_mode = SND_FF_ON_TIME;
	double ratio = 1.0;

	int framecount = (int)frame_count;

	int consumed = 0;
	int total_consumed_frames = 0;

	// printf("received %d audio frames\n", frame_count);

	// int full = 0;

	float remaining_space = snd.frame_count;
	pthread_mutex_lock(&audio_mutex);
	if (snd.frame_in >= snd.frame_out)
	{
		remaining_space = snd.frame_count - (snd.frame_in - snd.frame_out);
	}
	else
	{
		remaining_space = snd.frame_out - snd.frame_in;
	}
	pthread_mutex_unlock(&audio_mutex);
	// printf("    actual free: %g\n", remaining_space);
	perf.buffer_free = remaining_space;
	// let audio buffer fill up a little before playing audio, so no underruns occur. Target fill rate of buffer is about 50% so start playing when about 40% full
	if (perf.buffer_free < snd.frame_count * 0.6f) {
		SND_pauseAudio(false);
	} else if (perf.buffer_free > snd.frame_count * 0.99f) { // if for some reason buffer drops below 1% again, pause audio again (like psx core can stop sending audio in between scenes or after fast forward etc)
		SND_pauseAudio(true);
	}

	float tempdelay = ((snd.frame_count - remaining_space) / snd.sample_rate_out) * 1000;
	perf.buffer_ms = tempdelay;

	float occupancy = (float)(snd.frame_count - perf.buffer_free) / snd.frame_count;
	switch (current_mode)
	{
	case SND_FF_ON_TIME:
		if (occupancy > 0.65)
		{
			current_mode = SND_FF_LATE;
		}
		break;
	case SND_FF_LATE:
		if (occupancy > 0.85)
		{
			current_mode = SND_FF_VERY_LATE;
		}
		else if (occupancy < 0.25)
		{
			current_mode = SND_FF_ON_TIME;
		}
		break;
	case SND_FF_VERY_LATE:
		if (occupancy < 0.50)
		{
			current_mode = SND_FF_LATE;
		}
		break;
	}

	switch (current_mode)
	{
	case SND_FF_ON_TIME:
		ratio = 1.0;
		break;
	case SND_FF_LATE:
		ratio = 0.995;
		break;
	case SND_FF_VERY_LATE:
		// just drop the audio if its too late cause its never gonna catch up in fast forward
		return 0;
		// ratio = 0.980;
		// break;
	default:
		ratio = 1.0;
	}
	perf.ratio = ratio;

	while (framecount > 0)
	{

		int amount = MIN(BATCH_SIZE, framecount);

		for (int i = 0; i < amount; i++)
		{
			tmpbuffer[i] = frames[consumed + i];
		}
		consumed += amount;
		framecount -= amount;

		ResampledFrames resampled = resample_audio(
			tmpbuffer, amount, snd.sample_rate_in, snd.sample_rate_out, ratio);

		// Write resampled frames to the buffer
		int written_frames = 0;

		pthread_mutex_lock(&audio_mutex);
		for (int i = 0; i < resampled.frame_count; i++)
		{
			if ((snd.frame_in + 1) % snd.frame_count == snd.frame_out)
			{
				// Buffer is full, break. This should never happen tho, but just to be safe
				break;
			}
			snd.buffer[snd.frame_in] = resampled.frames[i];
			snd.frame_in = (snd.frame_in + 1) % snd.frame_count;
			written_frames++;
		}
		pthread_mutex_unlock(&audio_mutex);

		total_consumed_frames += written_frames;
		// No need to free - using static buffers
	}

	return total_consumed_frames;
}

void SND_init(double sample_rate, double frame_rate)
{ // plat_sound_init
	LOG_info("SND_init\n");
	if(SDL_WasInit(SDL_INIT_AUDIO))
		LOG_error("SND_init: already initialized\n");
	perf.req_fps = frame_rate;
	SDL_InitSubSystem(SDL_INIT_AUDIO);

	fps_counter = 0;
	fps_buffer_index = 0;

#if defined(USE_SDL2)
	LOG_info("Available audio drivers:\n");
	for (int i = 0; i < SDL_GetNumAudioDrivers(); i++)
	{
		LOG_info("- %s\n", SDL_GetAudioDriver(i));
	}
	LOG_info("Current audio driver: %s\n", SDL_GetCurrentAudioDriver());

	LOG_info("Available audio devices:\n");
	for (int i = 0; i < SDL_GetNumAudioDevices(0); i++)
	{
		LOG_info("- %s\n", SDL_GetAudioDeviceName(i, 0));
	}
#endif

	memset(&snd, 0, sizeof(struct SND_Context));
	snd.frame_rate = frame_rate;

	SDL_AudioSpec spec_in;
	SDL_AudioSpec spec_out;

	spec_in.freq = PLAT_pickSampleRate(sample_rate, MAX_SAMPLE_RATE);
	spec_in.format = AUDIO_S16;
	spec_in.channels = 2;
	spec_in.samples = SAMPLES;
	spec_in.callback = SND_audioCallback;

#if defined(USE_SDL2)
	snd.device_id = SDL_OpenAudioDevice(NULL, 0, &spec_in, &spec_out, SDL_AUDIO_ALLOW_ANY_CHANGE);
	if (snd.device_id <= 0)
	{
		LOG_info("SDL_OpenAudioDevice error: %s\n", SDL_GetError());
		if (SDL_OpenAudio(&spec_in, &spec_out) < 0) {
			LOG_info("SDL_OpenAudio error: %s\n", SDL_GetError());
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			return;
		}
	}
#else
	if (SDL_OpenAudio(&spec_in, &spec_out) < 0) {
		LOG_info("SDL_OpenAudio error: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return;
	}
	snd.device_id = 1;
#endif

	LOG_info("We now have audio device #%d\n", snd.device_id);

	snd.frame_count = ((float)spec_out.freq / SCREEN_FPS) * 8; // buffer size based on sample rate out (times 12 samples headroom)
	perf.buffer_size = snd.frame_count;
	snd.sample_rate_in = sample_rate;
	snd.sample_rate_out = spec_out.freq;
	perf.samplerate_in = snd.sample_rate_in;
	perf.samplerate_out = snd.sample_rate_out;

	SND_resizeBuffer();

	// start with audiodevice paused so buffer can fill a little, snd_batchsamples will unpause it
	SND_pauseAudio(true);
	LOG_info("sample rate: %i (req) %i (rec) [samples %i]\n", snd.sample_rate_in, snd.sample_rate_out, SAMPLES);
	snd.initialized = 1;

}

void SND_quit(void)
{
	if (!snd.initialized)
	{
		LOG_warn("Skipping SND teardown, not initialized.\n");
		return;
	}

	SND_pauseAudio(true);

#if defined(USE_SDL2)
	SDL_CloseAudioDevice(snd.device_id);
#else
	SDL_CloseAudio();
#endif

	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	if(SDL_WasInit(SDL_INIT_AUDIO))
		LOG_error("SND_quit: failed to quit audio!!\n");
	LOG_debug("SND_quit: quit audio!!\n");
	snd.initialized = 0;

	if (snd.buffer)
	{
		free(snd.buffer);
		snd.buffer = NULL;
	}
}

void SND_resetAudio(double sample_rate, double frame_rate)
{
	SND_quit();
	SND_init(sample_rate, frame_rate);
}

void SND_pauseAudio(bool paused)
{
#if defined(USE_SDL2)
	SDL_PauseAudioDevice(snd.device_id, paused);
#else
	SDL_PauseAudio(paused);
#endif
}

FALLBACK_IMPLEMENTATION void PLAT_audioDeviceWatchRegister(void (*cb)(int, int)) {}
FALLBACK_IMPLEMENTATION void PLAT_audioDeviceWatchUnregister(void) {}
FALLBACK_IMPLEMENTATION void PLAT_overrideMute(int mute) {}

///////////////////////////////

LID_Context lid = {
	.has_lid = 0,
	.is_open = 1,
};

FALLBACK_IMPLEMENTATION void PLAT_initLid(void) {}
FALLBACK_IMPLEMENTATION int PLAT_lidChanged(int *state) { return 0; }

///////////////////////////////

PAD_Context pad;

#define AXIS_DEADZONE 0x4000
void PAD_setAnalog(int neg_id, int pos_id, int value, int repeat_at)
{
	// LOG_info("neg %i pos %i value %i\n", neg_id, pos_id, value);
	int neg = 1 << neg_id;
	int pos = 1 << pos_id;
	if (value > AXIS_DEADZONE)
	{ // pressing
		if (!(pad.is_pressed & pos))
		{							  // not pressing
			pad.is_pressed |= pos;	  // set
			pad.just_pressed |= pos;  // set
			pad.just_repeated |= pos; // set
			pad.repeat_at[pos_id] = repeat_at;

			if (pad.is_pressed & neg)
			{							   // was pressing opposite
				pad.is_pressed &= ~neg;	   // unset
				pad.just_repeated &= ~neg; // unset
				pad.just_released |= neg;  // set
			}
		}
	}
	else if (value < -AXIS_DEADZONE)
	{ // pressing
		if (!(pad.is_pressed & neg))
		{							  // not pressing
			pad.is_pressed |= neg;	  // set
			pad.just_pressed |= neg;  // set
			pad.just_repeated |= neg; // set
			pad.repeat_at[neg_id] = repeat_at;

			if (pad.is_pressed & pos)
			{							   // was pressing opposite
				pad.is_pressed &= ~pos;	   // unset
				pad.just_repeated &= ~pos; // unset
				pad.just_released |= pos;  // set
			}
		}
	}
	else
	{ // not pressing
		if (pad.is_pressed & neg)
		{							  // was pressing
			pad.is_pressed &= ~neg;	  // unset
			pad.just_repeated &= neg; // unset
			pad.just_released |= neg; // set
		}
		if (pad.is_pressed & pos)
		{							  // was pressing
			pad.is_pressed &= ~pos;	  // unset
			pad.just_repeated &= pos; // unset
			pad.just_released |= pos; // set
		}
	}
}

void PAD_reset(void)
{
	// LOG_info("PAD_reset");
	pad.just_pressed = BTN_NONE;
	pad.is_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;
}
FALLBACK_IMPLEMENTATION void PLAT_pollInput(void)
{
	// reset transient state
	pad.just_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;

	uint32_t tick = SDL_GetTicks();
	for (int i = 0; i < BTN_ID_COUNT; i++)
	{
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick >= pad.repeat_at[i]))
		{
			pad.just_repeated |= btn; // set
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}

	// the actual poll
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		int btn = BTN_NONE;
		int pressed = 0; // 0=up,1=down
		int id = -1;
		if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
		{
			uint8_t code = event.key.keysym.scancode;
			pressed = event.type == SDL_KEYDOWN;
			// LOG_info("key event: %i (%i)\n", code,pressed);
			if (code == CODE_UP)
			{
				btn = BTN_DPAD_UP;
				id = BTN_ID_DPAD_UP;
			}
			else if (code == CODE_DOWN)
			{
				btn = BTN_DPAD_DOWN;
				id = BTN_ID_DPAD_DOWN;
			}
			else if (code == CODE_LEFT)
			{
				btn = BTN_DPAD_LEFT;
				id = BTN_ID_DPAD_LEFT;
			}
			else if (code == CODE_RIGHT)
			{
				btn = BTN_DPAD_RIGHT;
				id = BTN_ID_DPAD_RIGHT;
			}
			else if (code == CODE_A)
			{
				btn = BTN_A;
				id = BTN_ID_A;
			}
			else if (code == CODE_B)
			{
				btn = BTN_B;
				id = BTN_ID_B;
			}
			else if (code == CODE_X)
			{
				btn = BTN_X;
				id = BTN_ID_X;
			}
			else if (code == CODE_Y)
			{
				btn = BTN_Y;
				id = BTN_ID_Y;
			}
			else if (code == CODE_START)
			{
				btn = BTN_START;
				id = BTN_ID_START;
			}
			else if (code == CODE_SELECT)
			{
				btn = BTN_SELECT;
				id = BTN_ID_SELECT;
			}
			else if (code == CODE_MENU)
			{
				btn = BTN_MENU;
				id = BTN_ID_MENU;
			}
			else if (code == CODE_MENU_ALT)
			{
				btn = BTN_MENU;
				id = BTN_ID_MENU;
			}
			else if (code == CODE_L1)
			{
				btn = BTN_L1;
				id = BTN_ID_L1;
			}
			else if (code == CODE_L2)
			{
				btn = BTN_L2;
				id = BTN_ID_L2;
			}
			else if (code == CODE_L3)
			{
				btn = BTN_L3;
				id = BTN_ID_L3;
			}
			else if (code == CODE_R1)
			{
				btn = BTN_R1;
				id = BTN_ID_R1;
			}
			else if (code == CODE_R2)
			{
				btn = BTN_R2;
				id = BTN_ID_R2;
			}
			else if (code == CODE_R3)
			{
				btn = BTN_R3;
				id = BTN_ID_R3;
			}
			else if (code == CODE_PLUS)
			{
				btn = BTN_PLUS;
				id = BTN_ID_PLUS;
			}
			else if (code == CODE_MINUS)
			{
				btn = BTN_MINUS;
				id = BTN_ID_MINUS;
			}
			else if (code == CODE_POWER)
			{
				btn = BTN_POWER;
				id = BTN_ID_POWER;
			}
			else if (code == CODE_POWEROFF)
			{
				btn = BTN_POWEROFF;
				id = BTN_ID_POWEROFF;
			} // nano-only

			// Filter: ignore spurious +/- events that arrive immediately after resume. (mainly a miyoo flip issue)
			#define RESUME_IGNORE_MS 250
			if ((id == BTN_ID_PLUS || id == BTN_ID_MINUS) && pwr.resume_tick && (tick - pwr.resume_tick < RESUME_IGNORE_MS)) {
				//OG_info("ignoring spurious +/- press after resume (id=%d delta=%u)", id, tick - pwr.resume_tick);
				btn = BTN_NONE; // drop this event
			}
		}
		else if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP)
		{
			uint8_t joy = event.jbutton.button;
			pressed = event.type == SDL_JOYBUTTONDOWN;
			// LOG_info("joy event: %i (%i)\n", joy,pressed);
			if (joy == JOY_UP)
			{
				btn = BTN_DPAD_UP;
				id = BTN_ID_DPAD_UP;
			}
			else if (joy == JOY_DOWN)
			{
				btn = BTN_DPAD_DOWN;
				id = BTN_ID_DPAD_DOWN;
			}
			else if (joy == JOY_LEFT)
			{
				btn = BTN_DPAD_LEFT;
				id = BTN_ID_DPAD_LEFT;
			}
			else if (joy == JOY_RIGHT)
			{
				btn = BTN_DPAD_RIGHT;
				id = BTN_ID_DPAD_RIGHT;
			}
			else if (joy == JOY_A)
			{
				btn = BTN_A;
				id = BTN_ID_A;
			}
			else if (joy == JOY_B)
			{
				btn = BTN_B;
				id = BTN_ID_B;
			}
			else if (joy == JOY_X)
			{
				btn = BTN_X;
				id = BTN_ID_X;
			}
			else if (joy == JOY_Y)
			{
				btn = BTN_Y;
				id = BTN_ID_Y;
			}
			else if (joy == JOY_START)
			{
				btn = BTN_START;
				id = BTN_ID_START;
			}
			else if (joy == JOY_SELECT)
			{
				btn = BTN_SELECT;
				id = BTN_ID_SELECT;
			}
			else if (joy == JOY_MENU)
			{
				btn = BTN_MENU;
				id = BTN_ID_MENU;
			}
			else if (joy == JOY_MENU_ALT)
			{
				btn = BTN_MENU;
				id = BTN_ID_MENU;
			}
			else if (joy == JOY_MENU_ALT2)
			{
				btn = BTN_MENU;
				id = BTN_ID_MENU;
			}
			else if (joy == JOY_L1)
			{
				btn = BTN_L1;
				id = BTN_ID_L1;
			}
			else if (joy == JOY_L2)
			{
				btn = BTN_L2;
				id = BTN_ID_L2;
			}
			else if (joy == JOY_L3)
			{
				btn = BTN_L3;
				id = BTN_ID_L3;
			}
			else if (joy == JOY_R1)
			{
				btn = BTN_R1;
				id = BTN_ID_R1;
			}
			else if (joy == JOY_R2)
			{
				btn = BTN_R2;
				id = BTN_ID_R2;
			}
			else if (joy == JOY_R3)
			{
				btn = BTN_R3;
				id = BTN_ID_R3;
			}
			else if (joy == JOY_PLUS)
			{
				btn = BTN_PLUS;
				id = BTN_ID_PLUS;
			}
			else if (joy == JOY_MINUS)
			{
				btn = BTN_MINUS;
				id = BTN_ID_MINUS;
			}
			else if (joy == JOY_POWER)
			{
				btn = BTN_POWER;
				id = BTN_ID_POWER;
			}
		}
		else if (event.type == SDL_JOYHATMOTION)
		{
			int hats[4] = {-1, -1, -1, -1}; // -1=no change,0=up,1=down,2=left,3=right btn_ids
			int hat = event.jhat.value;
			// LOG_info("hat event: %i\n", hat);
			// TODO: safe to assume hats will always be the primary dpad?
			// TODO: this is literally a bitmask, make it one (oh, except there's 3 states...)
			switch (hat)
			{
			case SDL_HAT_UP:
				hats[0] = 1;
				hats[1] = 0;
				hats[2] = 0;
				hats[3] = 0;
				break;
			case SDL_HAT_DOWN:
				hats[0] = 0;
				hats[1] = 1;
				hats[2] = 0;
				hats[3] = 0;
				break;
			case SDL_HAT_LEFT:
				hats[0] = 0;
				hats[1] = 0;
				hats[2] = 1;
				hats[3] = 0;
				break;
			case SDL_HAT_RIGHT:
				hats[0] = 0;
				hats[1] = 0;
				hats[2] = 0;
				hats[3] = 1;
				break;
			case SDL_HAT_LEFTUP:
				hats[0] = 1;
				hats[1] = 0;
				hats[2] = 1;
				hats[3] = 0;
				break;
			case SDL_HAT_LEFTDOWN:
				hats[0] = 0;
				hats[1] = 1;
				hats[2] = 1;
				hats[3] = 0;
				break;
			case SDL_HAT_RIGHTUP:
				hats[0] = 1;
				hats[1] = 0;
				hats[2] = 0;
				hats[3] = 1;
				break;
			case SDL_HAT_RIGHTDOWN:
				hats[0] = 0;
				hats[1] = 1;
				hats[2] = 0;
				hats[3] = 1;
				break;
			case SDL_HAT_CENTERED:
				hats[0] = 0;
				hats[1] = 0;
				hats[2] = 0;
				hats[3] = 0;
				break;
			default:
				break;
			}

			for (id = 0; id < 4; id++)
			{
				int state = hats[id];
				btn = 1 << id;
				if (state == 0)
				{
					pad.is_pressed &= ~btn;	   // unset
					pad.just_repeated &= ~btn; // unset
					pad.just_released |= btn;  // set
				}
				else if (state == 1 && (pad.is_pressed & btn) == BTN_NONE)
				{
					pad.just_pressed |= btn;  // set
					pad.just_repeated |= btn; // set
					pad.is_pressed |= btn;	  // set
					pad.repeat_at[id] = tick + PAD_REPEAT_DELAY;
				}
			}
			btn = BTN_NONE; // already handled, force continue
		}
		else if (event.type == SDL_JOYAXISMOTION)
		{
			int axis = event.jaxis.axis;
			int val = event.jaxis.value;
			// LOG_info("axis: %i (%i)\n", axis,val);

			// triggers on tg5040
			if (axis == AXIS_L2)
			{
				btn = BTN_L2;
				id = BTN_ID_L2;
				pressed = val > 0;
			}
			else if (axis == AXIS_R2)
			{
				btn = BTN_R2;
				id = BTN_ID_R2;
				pressed = val > 0;
			}

			else if (axis == AXIS_LX)
			{
				pad.laxis.x = val;
				PAD_setAnalog(BTN_ID_ANALOG_LEFT, BTN_ID_ANALOG_RIGHT, val, tick + PAD_REPEAT_DELAY);
			}
			else if (axis == AXIS_LY)
			{
				pad.laxis.y = val;
				PAD_setAnalog(BTN_ID_ANALOG_UP, BTN_ID_ANALOG_DOWN, val, tick + PAD_REPEAT_DELAY);
			}
			else if (axis == AXIS_RX)
				pad.raxis.x = val;
			else if (axis == AXIS_RY)
				pad.raxis.y = val;

			// axis will fire off what looks like a release
			// before the first press but you can't release
			// a button that wasn't pressed
			if (!pressed && btn != BTN_NONE && !(pad.is_pressed & btn))
			{
				// LOG_info("cancel: %i\n", axis);
				btn = BTN_NONE;
			}
		}
		else if (event.type == SDL_QUIT)
		{
			PWR_powerOff(0);
		}
		else if (event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED)
		{
			PAD_update(&event);
		}

		if (btn == BTN_NONE)
			continue;

		if (!pressed)
		{
			pad.is_pressed &= ~btn;	   // unset
			pad.just_repeated &= ~btn; // unset
			pad.just_released |= btn;  // set
		}
		else if ((pad.is_pressed & btn) == BTN_NONE)
		{
			pad.just_pressed |= btn;  // set
			pad.just_repeated |= btn; // set
			pad.is_pressed |= btn;	  // set
			pad.repeat_at[id] = tick + PAD_REPEAT_DELAY;
		}
	}

	if (lid.has_lid && PLAT_lidChanged(NULL))
		pad.just_released |= BTN_SLEEP;
}
FALLBACK_IMPLEMENTATION int PLAT_shouldWake(void)
{
	int lid_open = 1; // assume open by default
	if (lid.has_lid && PLAT_lidChanged(&lid_open) && lid_open)
		return 1;

	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		if (event.type == SDL_KEYUP)
		{
			uint8_t code = event.key.keysym.scancode;
			if ((BTN_WAKE == BTN_POWER && code == CODE_POWER) || (BTN_WAKE == BTN_MENU && (code == CODE_MENU || code == CODE_MENU_ALT)))
			{
				// ignore input while lid is closed
				if (lid.has_lid && !lid.is_open)
					return 0; // do it here so we eat the input
				return 1;
			}
		}
		else if (event.type == SDL_JOYBUTTONUP)
		{
			uint8_t joy = event.jbutton.button;
			if ((BTN_WAKE == BTN_POWER && joy == JOY_POWER) || (BTN_WAKE == BTN_MENU && (joy == JOY_MENU || joy == JOY_MENU_ALT)))
			{
				// ignore input while lid is closed
				if (lid.has_lid && !lid.is_open)
					return 0; // do it here so we eat the input
				return 1;
			}
		}
	}
	return 0;
}
FALLBACK_IMPLEMENTATION int PLAT_supportsDeepSleep(void) { return 0; }
FALLBACK_IMPLEMENTATION int PLAT_deepSleep(void)
{
	const char *state_path = "/sys/power/state";
	int state_fd = 0;

	for (int i = 0; i < 5; i++) {

		// Check for power button press while waiting to retry
		uint32_t attempt_ticks  = SDL_GetTicks();
		if (i > 0) { // Don't wait on first attempt
			while (1) {
				if (pwr.requested_wake || PAD_wake()) {
					pwr.requested_wake = 0;
					return 0;
				}
				SDL_Delay(200);
				if (SDL_GetTicks() - attempt_ticks >= 2000) {
					break;
				}
			}
		}

		state_fd = open(state_path, O_WRONLY);
		if (state_fd < 0)
		{
			LOG_error("failed to open %s: %d\n", state_path, errno);
			LOG_info("retrying suspend in 2 seconds...\n");
			close(state_fd);
			continue;
		}

		LOG_info("suspending to RAM\n");
		int ret = write(state_fd, "mem", 3);
		if (ret < 0)
		{
			// Can fail shortly after resuming with EBUSY
			LOG_error("failed to set power state: %d\n", errno);
			LOG_info("retrying suspend in 2 seconds...\n");
			close(state_fd);
			continue;
		}

		LOG_info("returned from suspend\n");
		close(state_fd);
		return 0;
	}

	return -1;
}

int PAD_anyJustPressed(void) { return pad.just_pressed != BTN_NONE; }
int PAD_anyPressed(void) { return pad.is_pressed != BTN_NONE; }
int PAD_anyJustReleased(void) { return pad.just_released != BTN_NONE; }

int PAD_justPressed(int btn) { return pad.just_pressed & btn; }
int PAD_isPressed(int btn) { return pad.is_pressed & btn; }
int PAD_justReleased(int btn) { return pad.just_released & btn; }
int PAD_justRepeated(int btn) { return pad.just_repeated & btn; }

int PAD_tappedBtn(int btn, uint32_t now)
{
#define MENU_DELAY 250 // also in PWR_update()
	static uint32_t menu_start = 0;
	static int ignore_menu = 0;
	if (PAD_justPressed(btn))
	{
		ignore_menu = 0;
		menu_start = now;
	}
	else if (PAD_isPressed(btn) && BTN_MOD_BRIGHTNESS == btn && (PAD_justPressed(BTN_MOD_PLUS) || PAD_justPressed(BTN_MOD_MINUS)))
	{
		ignore_menu = 1;
	}
	return (!ignore_menu && PAD_justReleased(btn) && now - menu_start < MENU_DELAY);
}

int PAD_tappedMenu(uint32_t now)
{
	return PAD_tappedBtn(BTN_MENU, now);
}

int PAD_tappedSelect(uint32_t now)
{
	return PAD_tappedBtn(BTN_SELECT, now);
}

///////////////////////////////
static struct VIB_Context
{
	int initialized;
	pthread_t pt;
	int queued_strength;
	int strength;
} vib = {0};
static void *VIB_thread(void *arg)
{
#define DEFER_FRAMES 3
	static int defer = 0;
	while (1)
	{
		SDL_Delay(17);
		if (vib.queued_strength != vib.strength)
		{
			if (defer < DEFER_FRAMES && vib.queued_strength == 0)
			{ // minimize vacillation between 0 and some number (which this motor doesn't like)
				defer += 1;
				continue;
			}
			vib.strength = vib.queued_strength;
			defer = 0;

			PLAT_setRumble(vib.strength);
		}
	}
	return 0;
}
void VIB_init(void)
{
	vib.queued_strength = vib.strength = 0;
	pthread_create(&vib.pt, NULL, &VIB_thread, NULL);
	vib.initialized = 1;
}
void VIB_quit(void)
{
	if (!vib.initialized)
		return;

	VIB_setStrength(0);
	pthread_cancel(vib.pt);
	pthread_join(vib.pt, NULL);
}
void VIB_setStrength(int strength)
{
	if (vib.queued_strength == strength)
		return;
	vib.queued_strength = strength;
}
int VIB_getStrength(void)
{
	return vib.strength;
}

#define MIN_STRENGTH 0x0000
#define MAX_STRENGTH 0xFFFF
#define NUM_INCREMENTS 10

int VIB_scaleStrength(int strength)
{ // scale through 0-10 (NUM_INCREMENTS)
	int scaled_strength = MIN_STRENGTH + (int)(strength * ((long long)(MAX_STRENGTH - MIN_STRENGTH) / NUM_INCREMENTS));
	return scaled_strength; // between 0x0000 and 0xFFFF
}

void VIB_singlePulse(int strength, int duration_ms)
{
	VIB_setStrength(0);
	VIB_setStrength(VIB_scaleStrength(strength));
	usleep(duration_ms * 1000);
	VIB_setStrength(0);
}

void VIB_doublePulse(int strength, int duration_ms, int gap_ms)
{
	VIB_setStrength(0);
	VIB_singlePulse(VIB_scaleStrength(strength), duration_ms);
	usleep(gap_ms * 1000);
	VIB_setStrength(0);
	usleep(gap_ms * 1000);
	VIB_singlePulse(VIB_scaleStrength(strength), duration_ms);
	usleep(gap_ms * 1000);
	VIB_setStrength(0);
}

void VIB_triplePulse(int strength, int duration_ms, int gap_ms)
{
	VIB_setStrength(0);
	VIB_singlePulse(VIB_scaleStrength(strength), duration_ms);
	usleep(gap_ms * 1000);
	VIB_setStrength(0);
	usleep(gap_ms * 1000);
	VIB_singlePulse(VIB_scaleStrength(strength), duration_ms);
	usleep(gap_ms * 1000);
	VIB_setStrength(0);
	usleep(gap_ms * 1000);
	VIB_singlePulse(VIB_scaleStrength(strength), duration_ms);
	usleep(gap_ms * 1000);
	VIB_setStrength(0);
}

///////////////////////////////

static void PWR_updateBatteryStatus(void)
{
	int is_charging, charge;
	PLAT_getBatteryStatusFine(&is_charging, &charge);
	SDL_AtomicSet(&pwr.is_charging, is_charging);
	SDL_AtomicSet(&pwr.charge, charge);

	// this is technically redundant, but PWR_update() might not always be called to conserve battery and cycles
	LEDS_applyRules();
}

static void PWR_updateNetworkStatus(void)
{
	if (SDL_AtomicGet(&pwr.poll_network_status)) {
		int is_online;
		PLAT_getNetworkStatus(&is_online);
		SDL_AtomicSet(&pwr.is_online, is_online);
	}
}

void PWR_updateFrequency(int secs, int updateWifi)
{
	if (secs > 0)
		SDL_AtomicSet(&pwr.update_secs, secs);
	SDL_AtomicSet(&pwr.poll_network_status, updateWifi);
}

static void *PWR_monitorBattery(void *arg)
{
	while (1)
	{
		struct PWR_Context *pwr_ctx = (struct PWR_Context *)arg;
		int interval = SDL_AtomicGet(&pwr_ctx->update_secs);
		if (interval <= 0)
			interval = 1;
		sleep(interval);
		PWR_updateBatteryStatus();
		PWR_updateNetworkStatus();
	}
	return NULL;
}

void PWR_init(void)
{
	pwr.can_sleep = 1;
	pwr.can_poweroff = 1;
	pwr.can_autosleep = 1;

	pwr.requested_sleep = 0;
	pwr.requested_wake = 0;
	pwr.resume_tick = 0;

	SDL_AtomicSet(&pwr.charge, PWR_LOW_CHARGE);

	SDL_AtomicSet(&pwr.update_secs, 5);
	SDL_AtomicSet(&pwr.poll_network_status, 1);
	pwr.initialized = 1;

	if (CFG_getHaptics())
		VIB_singlePulse(VIB_bootStrength, VIB_bootDuration_ms);

	PWR_updateBatteryStatus();

	pthread_create(&pwr.battery_pt, NULL, &PWR_monitorBattery, &pwr);
	LOG_info("PWR_init complete\n");
}
void PWR_quit(void)
{
	if (!pwr.initialized)
		return;

	// cancel battery thread
	pthread_cancel(pwr.battery_pt);
	pthread_join(pwr.battery_pt, NULL);
}

int PWR_ignoreSettingInput(int btn, int show_setting)
{
	return show_setting && (btn == BTN_MOD_PLUS || btn == BTN_MOD_MINUS);
}

void PWR_update(int *_dirty, int *_show_setting, PWR_callback_t before_sleep, PWR_callback_t after_sleep)
{
	int dirty = _dirty ? *_dirty : 0;
	int show_setting = _show_setting ? *_show_setting : 0;

	static uint32_t last_input_at = 0;	   // timestamp of last input (autosleep)
	static uint32_t checked_charge_at = 0; // timestamp of last time checking charge
	static uint32_t setting_shown_at = 0;  // timestamp when settings started being shown
	static uint32_t power_pressed_at = 0;  // timestamp when power button was just pressed
	static uint32_t mod_unpressed_at = 0;  // timestamp of last time settings modifier key was NOT down
	static uint32_t was_muted = -1;
	if (was_muted == -1 && InitializedSettings())
		was_muted = GetMute();

	static int was_charging = -1;
	if (was_charging == -1) was_charging = SDL_AtomicGet(&pwr.is_charging);

	uint32_t now = SDL_GetTicks();
	if (was_charging || PAD_anyPressed() || last_input_at == 0)
		last_input_at = now;

#define CHARGE_DELAY 1000
	if (dirty || now - checked_charge_at >= CHARGE_DELAY)
	{
		int is_charging = SDL_AtomicGet(&pwr.is_charging);
		if (was_charging != is_charging)
		{
			was_charging = is_charging;
			dirty = 1;
		}
		checked_charge_at = now;
	}

	if (PAD_justReleased(BTN_POWEROFF) || (power_pressed_at && now - power_pressed_at >= 1000))
	{
		if (before_sleep)
			before_sleep();
		system("gametimectl.elf stop_all");
		PWR_powerOff(0);
	}

	if (PAD_justPressed(BTN_POWER))
	{
		if (now - pwr.resume_tick < 1000)
		{
			LOG_debug("ignoring spurious power button press (just resumed)\n");
			power_pressed_at = 0;
		}
		else
		{
			power_pressed_at = now;
		}
	}

	const int screenOffDelay = CFG_getScreenTimeoutSecs() * 1000;
	if (screenOffDelay == 0 || (now - last_input_at >= screenOffDelay && PWR_preventAutosleep()))
		last_input_at = now;

	if (
		pwr.requested_sleep ||											   // hardware requested sleep
		(screenOffDelay > 0 && now - last_input_at >= screenOffDelay) ||   // autosleep
		(pwr.can_sleep && PAD_justReleased(BTN_SLEEP) && (power_pressed_at || !lid.is_open)) // manual sleep
	)
	{
		pwr.requested_sleep = 0;
		if (before_sleep)
			before_sleep();
		PWR_sleep();
		if (after_sleep)
			after_sleep();
		last_input_at = now = SDL_GetTicks();
		power_pressed_at = 0;
		dirty = 1;
	}

	int was_dirty = dirty; // dirty list (not including settings/battery)

	// TODO: only delay hiding setting changes if that setting didn't require a modifier button be held, otherwise release as soon as modifier is released

	int delay_settings = BTN_MOD_BRIGHTNESS == BTN_MENU; // when both volume and brighness require a modifier hide settings as soon as it is released
#define SETTING_DELAY 500
	if (show_setting && (now - setting_shown_at >= SETTING_DELAY || !delay_settings) && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS) && !PAD_isPressed(BTN_MOD_COLORTEMP))
	{
		show_setting = 0;
		dirty = 1;
	}

	if (!show_setting && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS) && !PAD_isPressed(BTN_MOD_COLORTEMP))
	{
		mod_unpressed_at = now; // this feels backwards but is correct
	}

#define MOD_DELAY 250
	if (
		(
			(PAD_isPressed(BTN_MOD_VOLUME) || PAD_isPressed(BTN_MOD_BRIGHTNESS) || PAD_isPressed(BTN_MOD_COLORTEMP)) &&
			(!delay_settings || now - mod_unpressed_at >= MOD_DELAY)) ||
		((!BTN_MOD_VOLUME || !BTN_MOD_BRIGHTNESS || !BTN_MOD_COLORTEMP) && (PAD_justRepeated(BTN_MOD_PLUS) || PAD_justRepeated(BTN_MOD_MINUS))))
	{
		setting_shown_at = now;
		if (PAD_isPressed(BTN_MOD_BRIGHTNESS))
		{
			show_setting = 1;
		}
		else if (PAD_isPressed(BTN_MOD_COLORTEMP))
		{
			show_setting = 3;
		}
		else
		{
			show_setting = 2;
		}
	}

	if (InitializedSettings())
	{
		int muted = GetMute();
		if (muted != was_muted)
		{
			was_muted = muted;
			show_setting = 2;
			setting_shown_at = now;
		}
	}

	LEDS_applyRules();

	if (show_setting)
		dirty = 1; // shm is slow or keymon is catching input on the next frame
	if (_dirty)
		*_dirty = dirty;
	if (_show_setting)
		*_show_setting = show_setting;
}

// TODO: this isn't whether it can sleep but more if it should sleep in response to the sleep button
void PWR_disableSleep(void)
{
	pwr.can_sleep = 0;
}
void PWR_enableSleep(void)
{
	pwr.can_sleep = 1;
}

void PWR_disablePowerOff(void)
{
	pwr.can_poweroff = 0;
}
void PWR_powerOff(int reboot)
{
	if (pwr.can_poweroff)
	{

		int w = FIXED_WIDTH;
		int h = FIXED_HEIGHT;
		int p = FIXED_PITCH;
		if (GetHDMI())
		{
			w = HDMI_WIDTH;
			h = HDMI_HEIGHT;
			p = HDMI_PITCH;
		}
		gfx.screen = GFX_resize(w, h, p);

		char *msg;
		if (HAS_POWER_BUTTON || HAS_POWEROFF_BUTTON)
		{
			if (exists(AUTO_RESUME_PATH))
				msg = (char *)"Quicksave created,\npowering off";
			else if (reboot > 0)
				msg = (char *)"Rebooting";
			else
				msg = (char *)"Powering off";
		}
		else
		{
			msg = exists(AUTO_RESUME_PATH) ? (char *)"Quicksave created,\npower off now" : (char *)"Power off now";
		}

		// LOG_info("PWR_powerOff %s (%ix%i)\n", gfx.screen, gfx.screen->w, gfx.screen->h);

		// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...

		PLAT_clearLayers(0);
		PLAT_clearAll();
		GFX_blitMessage(font.large, msg, gfx.screen, &(SDL_Rect){0, 0, gfx.screen->w, gfx.screen->h}); //, NULL);
		GFX_flip(gfx.screen);

		system("killall -TERM keymon.elf");
		system("killall -TERM batmon.elf");
		system("killall -TERM audiomon.elf");

		PWR_updateFrequency(-1, false);

		PLAT_powerOff(reboot);
	}
}

static void PWR_enterSleep(void)
{
	SND_pauseAudio(true);
	LEDS_pushProfileOverride(LIGHT_PROFILE_SLEEP);
	if (GetHDMI())
	{
		PLAT_clearVideo(gfx.screen);
		PLAT_flip(gfx.screen, 0);
	}
	else
	{
		SetRawVolume(MUTE_VOLUME_RAW);
		if (CFG_getHaptics())
		{
			VIB_singlePulse(VIB_sleepStrength, VIB_sleepDuration_ms);
		}
		PLAT_enableBacklight(0);
	}
	system("killall -STOP keymon.elf");
	system("killall -STOP batmon.elf");
	system("killall -STOP audiomon.elf");

	PWR_updateFrequency(-1, false);

	sync();
}
static void PWR_exitSleep(void)
{
	LEDS_popProfileOverride(LIGHT_PROFILE_SLEEP);

	PWR_updateFrequency(-1, true);

	system("killall -CONT keymon.elf");
	system("killall -CONT batmon.elf");
	system("killall -CONT audiomon.elf");

	if (GetHDMI())
	{
		// buh
	}
	else
	{
		if (CFG_getHaptics())
		{
			VIB_singlePulse(VIB_sleepStrength, VIB_sleepDuration_ms);
		}
		PLAT_enableBacklight(1);
		SND_overrideMute(1);
		SetVolume(GetVolume());
	}
	// reinitialize audio after sleep otherwise it doesnt come back on sometimes
	LOG_info("Reinitialize audio after sleep\n");
	SND_resetAudio(snd.sample_rate_in, snd.frame_rate);

	sync(); // why?
}

static void PWR_waitForWake(void)
{
	uint32_t sleep_ticks = SDL_GetTicks();
	int deep_sleep_attempts = 0;
	const int sleepDelay = CFG_getSuspendTimeoutSecs() * 1000;
	while (!PAD_wake())
	{
		if (pwr.requested_wake)
		{
			pwr.requested_wake = 0;
			break;
		}
		if (sleepDelay > 0)
		{
			SDL_Delay(200);
			if (SDL_GetTicks() - sleep_ticks >= sleepDelay)
			{ // increased to two minutes
				if (SDL_AtomicGet(&pwr.is_charging))
				{
					sleep_ticks += 60000; // check again in a minute
					continue;
				}
				if (PLAT_supportsDeepSleep())
				{
					int ret = PWR_deepSleep();
					if (ret == 0)
					{
						return;
					}
					else
					{
						LOG_warn("failed to enter deep sleep - powering off\n");
					}
				}
				if (pwr.can_poweroff)
				{
					PWR_powerOff(0);
				}
			}
		}
	}

	return;
}
void PWR_sleep(void)
{
	LOG_info("Entering hybrid sleep\n");

	system("gametimectl.elf stop_all");

	GFX_clear(gfx.screen);
	PAD_reset();
	PWR_enterSleep();
	PWR_waitForWake();
	PWR_exitSleep();
	PAD_reset();

	system("gametimectl.elf resume");

	pwr.resume_tick = SDL_GetTicks();
}

int PWR_deepSleep(void)
{
	// Run `${BIN_PATH}/suspend` if it exists, then fall back
	// to the PLAT_deepSleep implementation.
	//
	// We assume the suspend executable exits after a full
	// suspend/resume cycle.
	char *suspend_path = BIN_PATH "/suspend";
	if (exists(suspend_path))
	{
		LOG_info("suspending using platform suspend executable\n");

		int ret = system(suspend_path);
		if (ret < 0)
		{
			LOG_error("failed to launch suspend executable: %d\n", errno);
			return -1;
		}

		LOG_info("suspend executable exited with %d\n", ret);
		return ret == 0 ? 0 : -1;
	}

	return PLAT_deepSleep();
}

void PWR_disableAutosleep(void)
{
	pwr.can_autosleep = 0;
}
void PWR_enableAutosleep(void)
{
	pwr.can_autosleep = 1;
}
int PWR_preventAutosleep(void)
{
	return SDL_AtomicGet(&pwr.is_charging) || !pwr.can_autosleep || GetHDMI();
}

// updated by PWR_updateBatteryStatus()
int PWR_isCharging(void)
{
	return SDL_AtomicGet(&pwr.is_charging);
}
int PWR_getBattery(void)
{ // 10-100 in 10-20% fragments
	return SDL_AtomicGet(&pwr.charge);
}

int PWR_isOnline(void)
{
	return SDL_AtomicGet(&pwr.is_online);
}

///////////////////////////////

// TODO: tmp? move to individual platforms or allow overriding like PAD_poll/PAD_wake?
FALLBACK_IMPLEMENTATION int PLAT_setDateTime(int y, int m, int d, int h, int i, int s)
{
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock --utc -w", y, m, d, h, i, s);
	system(cmd);
	return 0; // why does this return an int?
}

///////////////////////////////
// RGB LED cruft

FALLBACK_IMPLEMENTATION void PLAT_initLeds(LightSettings *lights) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedBrightness(LightSettings *led) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedEffect(LightSettings *led) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedColor(LightSettings *led) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedInbrightness(LightSettings *led) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedEffectCycles(LightSettings *led) {}
FALLBACK_IMPLEMENTATION void PLAT_setLedEffectSpeed(LightSettings *led) {}

void LEDS_setProfile(int profile)
{
	if(lights_initialized == 0)
		return;

	LightSettings *new_lights = NULL;
	bool indicator = true;

	switch(profile)
	{
		case LIGHT_PROFILE_DEFAULT:
			new_lights = lightsDefault;
			indicator = false;
			break;
		case LIGHT_PROFILE_OFF:
			new_lights = lightsOff;
			indicator = false;
			break;
		case LIGHT_PROFILE_LOW_BATTERY:
			new_lights = lightsLowBattery;
			break;
		case LIGHT_PROFILE_CRITICAL_BATTERY:
			new_lights = lightsCriticalBattery;
			break;
		case LIGHT_PROFILE_CHARGING:
			new_lights = lightsCharging;
			break;
		case LIGHT_PROFILE_SLEEP:
			new_lights = lightsSleep;
			break;
		case LIGHT_PROFILE_AMBIENT:
			new_lights = lightsAmbient;
			indicator = false;
			break;
		default:
			return;
	}
	if (profile != LIGHT_PROFILE_AMBIENT && lights == (LightSettings (*)[MAX_LIGHTS])new_lights)
		return;
	lights = (LightSettings (*)[MAX_LIGHTS])new_lights;

	LEDS_updateLeds(indicator);
}

void LEDS_applyRules()
{
	if(lights_initialized == 0) {
		LOG_error("LEDS_applyRules: lights not initialized, skipping\n");
		return;
	}

	// some rules rely on pwr.is_charging and pwr.charge being valid
	if(pwr.initialized == 0)
		LOG_warn("LEDS_applyRules called before PWR_init\n");
	// some rules rely in InitSettings() being called (e.g GetMute())
	if(!InitializedSettings())
		LOG_warn("LEDS_applyRules called before InitSettings\n");

	// these are defined in order of priority, not necessarily in the order
	// of LightProfile enum.
	// e.g.
	// - if charging and low battery, charging takes priority
	if (pwr.initialized && SDL_AtomicGet(&pwr.is_charging)) {
		//LOG_info("LEDS_applyRules: charging\n");
		LEDS_setProfile(LIGHT_PROFILE_CHARGING);
	}
	// - if critical battery, critical battery takes priority over everything
	else if (pwr.initialized && SDL_AtomicGet(&pwr.charge) < PWR_LOW_CHARGE) {
		//LOG_info("LEDS_applyRules: critical battery\n");
		LEDS_setProfile(LIGHT_PROFILE_CRITICAL_BATTERY);
	}
	// - if muted, muted takes priority over everything except critical battery
	else if(InitializedSettings() && CFG_getMuteLEDs() && GetMute()) {
		//LOG_info("LEDS_applyRules: muted\n");
		LEDS_setProfile(LIGHT_PROFILE_OFF);
	}
	// other rules
	else if (pwr.initialized && SDL_AtomicGet(&pwr.charge) < PWR_LOW_CHARGE + 10 && SDL_AtomicGet(&pwr.charge) >= PWR_LOW_CHARGE) {
		//LOG_info("LEDS_applyRules: low battery\n");
		LEDS_setProfile(LIGHT_PROFILE_LOW_BATTERY);
	}
	// - if no other rule applies, use default (or temporary override)
	// manual rules to be set on demand: LIGHT_PROFILE_SLEEP, LIGHT_PROFILE_AMBIENT
	else {
		//LOG_info("LEDS_applyRules: default/override: %i\n", LEDS_getProfileOverride());
		LEDS_setProfile(LEDS_getProfileOverride());
	}
}

void LEDS_updateLeds(bool indicator_only)
{
	if(lights_initialized == 0) {
		LOG_error("LEDS_updateLeds: lights not initialized, skipping\n");
		return;
	}

	int lightsize = 3;
	char *device = getenv("DEVICE");
	int is_brick = exactMatch("brick", device);
	if (is_brick)
		lightsize = 4;
	if(!lights)
	{
		LOG_error("LEDS_updateLeds called but lights is NULL\n");
		return;
	}
	for (int i = 0; i < lightsize; i++)
	{
		// set brightness of each led
		if(indicator_only)
			PLAT_setLedInbrightness(&(*lights)[i]);
		else
			PLAT_setLedBrightness(&(*lights)[i]);

		PLAT_setLedEffectCycles(&(*lights)[i]); // set how many times animation should loop
		PLAT_setLedEffectSpeed(&(*lights)[i]);	// set animation speed
		PLAT_setLedColor(&(*lights)[i]);		// set color
		PLAT_setLedEffect(&(*lights)[i]);		// finally set the effect, on trimui devices this also applies the settings
	}
}

void LEDS_initLeds()
{
	PLAT_initLeds(lightsDefault);

	if(!lightsDefault)
	{
		LOG_error("LEDS_initLeds called but lightsDefault is NULL\n");
		return;
	}

	int lightsize = sizeof(lightsDefault) / sizeof(LightSettings);
	for (int i = 0; i < lightsize; i++)
	{
		// LIGHT_PROFILE_OFF
		lightsOff[i] = lightsDefault[i];
		lightsOff[i].brightness = 0;
		lightsOff[i].inbrightness = 0;

		// LIGHT_PROFILE_LOW_BATTERY
		lightsLowBattery[i] = lightsDefault[i];
		lightsLowBattery[i].effect = 3; // blink
		lightsLowBattery[i].color1 = 0xFF3300;
		lightsLowBattery[i].cycles = -1; // infinite

		// LIGHT_PROFILE_CRITICAL_BATTERY
		lightsCriticalBattery[i] = lightsDefault[i];
		lightsCriticalBattery[i].effect = 3; // blink
		lightsCriticalBattery[i].color1 = 0xFF0000;
		lightsCriticalBattery[i].cycles = -1; // infinite

		// LIGHT_PROFILE_CHARGING
		lightsCharging[i] = lightsDefault[i];
		lightsCharging[i].effect = 2; // breathe
		lightsCharging[i].color1 = 0x00FF00;
		lightsCharging[i].cycles = -1; // infinite

		// LIGHT_PROFILE_SLEEP
		lightsSleep[i] = lightsDefault[i];
		lightsSleep[i].effect = 2; // breathe
		lightsSleep[i].cycles = 5;

		// LIGHT_PROFILE_AMBIENT
		// just to have sensible defaults, will be updated by GFX_setAmbientColor
		lightsAmbient[i] = lightsDefault[i];
	}

	// this might be called more than once (for reasons), so reset to consistent state
	profile_override_top = -1;

	lights_initialized = 1;

	// once is enough to get us started, will be called by the main loop afterwards
	LEDS_applyRules();
}

bool LEDS_pushProfileOverride(int profile)
{
	if(profile_override_top == PROFILE_OVERRIDE_SIZE - 1)
	{
		LOG_debug("LED_profile stack is full, ignoring.\n");
		return false;
	}
	profile_override[++profile_override_top] = profile;
	LEDS_applyRules();

	return true;
}

bool LEDS_popProfileOverride(int profile)
{
	if(profile_override_top == -1) {
		LOG_debug("LED_profile stack is empty, nothing to pop.\n");
		return false;
	}
	if(LEDS_getProfileOverride() != profile) {
		LOG_debug("LEDS_popProfileOverride attempted to remove %d, but top of stack is %d\n", profile, LEDS_getProfileOverride());
		return false;
	}
	profile_override_top--;
	LEDS_applyRules();

	return true;
}

// returns top of stack, or default if stack is empty
int LEDS_getProfileOverride()
{
	if(profile_override_top == -1) {
		LOG_debug("LED_profile stack is empty, returning default profile.\n");
		return LIGHT_PROFILE_DEFAULT;
	}
	
	LOG_debug("LEDS_getProfileOverride: %i\n", profile_override[profile_override_top]);
	return profile_override[profile_override_top];
}

/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION bool PLAT_canTurbo(void) { return false; }
FALLBACK_IMPLEMENTATION int PLAT_toggleTurbo(int btn_id) { return 0; }
FALLBACK_IMPLEMENTATION void PLAT_clearTurbo() {}
FALLBACK_IMPLEMENTATION void PLAT_updateInput(const SDL_Event *event) {}

/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION FILE *PLAT_OpenSettings(const char *filename)
{
	char diskfilename[256];
	snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/%s", filename);

	FILE *file = fopen(diskfilename, "r");
	if (file == NULL)
	{
		return NULL;
	}
	return file;
}

FALLBACK_IMPLEMENTATION FILE *PLAT_WriteSettings(const char *filename)
{
	char diskfilename[256];
	snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/%s", filename);

	FILE *file = fopen(diskfilename, "w");
	if (file == NULL)
	{
		return NULL;
	}
	return file;
}
/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION void PLAT_initPlatform(void) {}

/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION void PLAT_initTimezones() {}
FALLBACK_IMPLEMENTATION void PLAT_getTimezones(char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH], int *tz_count) { tz_count = 0; }
FALLBACK_IMPLEMENTATION char *PLAT_getCurrentTimezone() { return "Foo/Bar"; }
FALLBACK_IMPLEMENTATION void PLAT_setCurrentTimezone(const char *tz) {}
FALLBACK_IMPLEMENTATION bool PLAT_getNetworkTimeSync(void) { return true; }
FALLBACK_IMPLEMENTATION void PLAT_setNetworkTimeSync(bool on) {}

/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION void PLAT_wifiInit() {}
FALLBACK_IMPLEMENTATION bool PLAT_hasWifi() { return false; }
FALLBACK_IMPLEMENTATION bool PLAT_wifiEnabled() { return false; }
FALLBACK_IMPLEMENTATION void PLAT_wifiEnable(bool on) {}

FALLBACK_IMPLEMENTATION int PLAT_wifiScan(struct WIFI_network *networks, int max) { return 0; }
FALLBACK_IMPLEMENTATION bool PLAT_wifiConnected() { return false; }
FALLBACK_IMPLEMENTATION int PLAT_wifiConnection(struct WIFI_connection *connection_info) { return 0; }
FALLBACK_IMPLEMENTATION bool PLAT_wifiHasCredentials(char *ssid, WifiSecurityType sec) { return false; }
FALLBACK_IMPLEMENTATION void PLAT_wifiForget(char *ssid, WifiSecurityType sec) {}
FALLBACK_IMPLEMENTATION void PLAT_wifiConnect(char *ssid, WifiSecurityType sec) {}
FALLBACK_IMPLEMENTATION void PLAT_wifiConnectPass(const char *ssid, WifiSecurityType sec, const char *pass) {}
FALLBACK_IMPLEMENTATION void PLAT_wifiDisconnect() {}
FALLBACK_IMPLEMENTATION bool PLAT_wifiDiagnosticsEnabled() { return false; }
FALLBACK_IMPLEMENTATION void PLAT_wifiDiagnosticsEnable(bool on) {}

/////////////////////////////////////////////////////////////////////////////////////////

FALLBACK_IMPLEMENTATION void PLAT_bluetoothInit() {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothDeinit() {}
FALLBACK_IMPLEMENTATION bool PLAT_hasBluetooth() { return false; }
FALLBACK_IMPLEMENTATION bool PLAT_bluetoothEnabled() { return false; }
FALLBACK_IMPLEMENTATION void PLAT_bluetoothEnable(bool on) {}
FALLBACK_IMPLEMENTATION bool PLAT_bluetoothDiagnosticsEnabled() { return false; }
FALLBACK_IMPLEMENTATION void PLAT_bluetoothDiagnosticsEnable(bool on) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothDiscovery(int on) {}
FALLBACK_IMPLEMENTATION bool PLAT_bluetoothDiscovering() { return false; }
FALLBACK_IMPLEMENTATION int PLAT_bluetoothScan(struct BT_device *devices, int max) { return 0; }
FALLBACK_IMPLEMENTATION int PLAT_bluetoothPaired(struct BT_devicePaired *devices, int max) { return 0; }
FALLBACK_IMPLEMENTATION void PLAT_bluetoothPair(char *addr) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothUnpair(char *addr) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothConnect(char *addr) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothDisconnect(char *addr) {}
FALLBACK_IMPLEMENTATION bool PLAT_bluetoothConnected() { return false; }
FALLBACK_IMPLEMENTATION bool PLAT_btIsConnected(void) { return PLAT_bluetoothConnected(); }
FALLBACK_IMPLEMENTATION void PLAT_bluetoothStreamInit(int ch, int samplerate) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothStreamBegin(int buffersize) {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothStreamEnd() {}
FALLBACK_IMPLEMENTATION void PLAT_bluetoothStreamQuit() {}
FALLBACK_IMPLEMENTATION int PLAT_bluetoothVolume() { return 100; }
FALLBACK_IMPLEMENTATION void PLAT_bluetoothSetVolume(int vol) {}

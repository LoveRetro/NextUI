#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <msettings.h>

#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <libgen.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <zip.h> 
#include <pthread.h>
#include <glob.h>
#include <lz4.h>

// libretro-common
#include "libretro.h"
#ifdef HAS_SRM
#include "streams/rzip_stream.h"
#include "streams/file_stream.h"
#endif

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "scaler.h"
#include "notification.h"
#include "config.h"
#include "ra_integration.h"
#include "ra_badges.h"
#include <dirent.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL.h>
#include <rcheevos/rc_client.h>

#include "minarch_internal.h"
#include "minarch_cheats.h"
#include "minarch_audio.h"
#include "minarch_input.h"
#include "minarch_options.h"
#include "minarch_frontend_opts.h"
#include "minarch_saves.h"
#include "minarch_video.h"
#include "minarch_core.h"
#include "minarch_game.h"
#include "minarch_environment.h"
#include "minarch_config.h"

///////////////////////////////////////

SDL_Surface* screen;
int quit = 0;
int newScreenshot = 0;
int show_menu = 0;
int simple_mode = 0;
enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

// default frontend options
int screen_scaling = SCALE_ASPECT;
int resampling_quality = 2;
int ambient_mode = 0;
int screen_sharpness = SHARPNESS_SOFT;
int screen_effect = EFFECT_NONE;
int cfg_screenx = 64;
int cfg_screeny = 64;
int overlay = 0; 
int use_core_fps = 0;
int sync_ref = 0;
int show_debug = 0;
int max_ff_speed = 3; // 4x
int ff_audio = 0;
int fast_forward = 0;
int rewind_pressed = 0;
int rewind_toggle = 0;
int last_rewind_pressed = 0;
int ff_toggled = 0;
int ff_hold_active = 0;
int ff_paused_by_rewind_hold = 0;
int rewinding = 0;
int rewind_cfg_enable = MINARCH_DEFAULT_REWIND_ENABLE;
int rewind_cfg_buffer_mb = MINARCH_DEFAULT_REWIND_BUFFER_MB;
int rewind_cfg_granularity = MINARCH_DEFAULT_REWIND_GRANULARITY;
int rewind_cfg_audio = MINARCH_DEFAULT_REWIND_AUDIO;
int rewind_cfg_compress = 1;
int rewind_cfg_lz4_acceleration = MINARCH_DEFAULT_REWIND_LZ4_ACCELERATION;
int overclock = 0; // auto
int has_custom_controllers = 0;
int gamepad_type = 0; // index in gamepad_labels/gamepad_values

// these are no longer constants as of the RG CubeXX (even though they look like it)
int DEVICE_WIDTH = 0;
int DEVICE_HEIGHT = 0;
int DEVICE_PITCH = 0;
int shader_reset_suppressed = 0;

GFX_Renderer renderer;

///////////////////////////////////////

struct Core core;



char* onoff_labels[] = {
	"Off",
	"On",
	NULL
};
char* scaling_labels[] = {
	"Native",
	"Aspect",
	"Aspect Screen",
	"Fullscreen",
	"Cropped",
	NULL
};
static char* resample_labels[] = {
	"Low",
	"Medium",
	"High",
	"Max",
	NULL
};
static char* rewind_enable_labels[] = {
	"Off",
	"On",
	NULL
};
static char* rewind_buffer_labels[] = {
	"8",
	"16",
	"32",
	"64",
	"128",
	"256",
	NULL
};
static char* rewind_granularity_values[] = {
	"16",
	"22",
	"25",
	"33",
	"50",
	"66",
	"100",
	"150",
	"200",
	"300",
	"450",
	"600",
	NULL
};
static char* rewind_granularity_labels[] = {
	"16 ms (~60 fps)",
	"22 ms (~45 fps)",
	"25 ms (~40 fps)",
	"33 ms (~30 fps)",
	"50 ms (~20 fps)",
	"66 ms (~15 fps)",
	"100 ms (~10 fps)",
	"150 ms (~7 fps)",
	"200 ms (~5 fps)",
	"300 ms",
	"450 ms",
	"600 ms",
	NULL
};
static char* rewind_compression_accel_values[] = {
	"1",
	"2",
	"4",
	"8",
	"12",
	NULL
};
static char* rewind_compression_accel_labels[] = {
	"1 (best ratio)",
	"2 (default)",
	"4 (fast)",
	"8 (faster)",
	"12 (fastest)",
	NULL
};
static char* ambient_labels[] = {
	"Off",
	"All",
	"Top",
	"FN",
	"LR",
	"Top/LR",
	NULL
};

static char* effect_labels[] = {
	"None",
	"Line",
	"Grid",
	NULL
};
static char* overlay_labels[] = {
	"None",
	NULL
};
// static char* sharpness_labels[] = {
// 	"Sharp",
// 	"Crisp",
// 	"Soft",
// 	NULL
// };
static char* sharpness_labels[] = {
	"NEAREST",
	"LINEAR",
	NULL
};
static char* sync_ref_labels[] = {
	"Auto",
	"Screen",
	"Native",
	NULL
};
static char* max_ff_labels[] = {
	"None",
	"2x",
	"3x",
	"4x",
	"5x",
	"6x",
	"7x",
	"8x",
	NULL,
};
static char* offset_labels[] = {
	"-64",
	"-63",
	"-62",
	"-61",
	"-60",
	"-59",
	"-58",
	"-57",
	"-56",
	"-55",
	"-54",
	"-53",
	"-52",
	"-51",
	"-50",
	"-49",
	"-48",
	"-47",
	"-46",
	"-45",
	"-44",
	"-43",
	"-42",
	"-41",
	"-40",
	"-39",
	"-38",
	"-37",
	"-36",
	"-35",
	"-34",
	"-33",
	"-32",
	"-31",
	"-30",
	"-29",
	"-28",
	"-27",
	"-26",
	"-25",
	"-24",
	"-23",
	"-22",
	"-21",
	"-20",
	"-19",
	"-18",
	"-17",
	"-16",
	"-15",
	"-14",
	"-13",
	"-12",
	"-11",
	"-10",
	"-9",
	"-8",
	"-7",
	"-6",
	"-5",
	"-4",
	"-3",
	"-2",
	"-1",
	"0",
	"1",
	"2",
	"3",
	"4",
	"5",
	"6",
	"7",
	"8",
	"9",
	"10",
	"11",
	"12",
	"13",
	"14",
	"15",
	"16",
	"17",
	"18",
	"19",
	"20",
	"21",
	"22",
	"23",
	"24",
	"25",
	"26",
	"27",
	"28",
	"29",
	"30",
	"31",
	"32",
	"33",
	"34",
	"35",
	"36",
	"37",
	"38",
	"39",
	"40",
	"41",
	"42",
	"43",
	"44",
	"45",
	"46",
	"47",
	"48",
	"49",
	"50",
	"51",
	"52",
	"53",
	"54",
	"55",
	"56",
	"57",
	"58",
	"59",
	"60",
	"61",
	"62",
	"63",
	"64",
	NULL,
};
static char* nrofshaders_labels[] = {
	"off",
	"1",
	"2",
	"3",
	NULL
};
static char* shupscale_labels[] = {
	"1",
	"2",
	"3",
	"4",
	"5",
	"6",
	"7",
	"8",
	"screen",
	NULL
};
static char* shfilter_labels[] = {
	"NEAREST",
	"LINEAR",
	NULL
};
static char* shscaletype_labels[] = {
	"source",
	"relative",
	NULL
};

///////////////////////////////


enum {
	SYNC_SRC_AUTO,
	SYNC_SRC_SCREEN,
	SYNC_SRC_CORE
};
// SH_EXTRASETTINGS..SH_NONE defined in minarch_internal.h


ButtonMapping default_button_mapping[] = { // used if pak.cfg doesn't exist or doesn't have bindings
	{"Up",			RETRO_DEVICE_ID_JOYPAD_UP,		BTN_ID_DPAD_UP},
	{"Down",		RETRO_DEVICE_ID_JOYPAD_DOWN,	BTN_ID_DPAD_DOWN},
	{"Left",		RETRO_DEVICE_ID_JOYPAD_LEFT,	BTN_ID_DPAD_LEFT},
	{"Right",		RETRO_DEVICE_ID_JOYPAD_RIGHT,	BTN_ID_DPAD_RIGHT},
	{"A Button",	RETRO_DEVICE_ID_JOYPAD_A,		BTN_ID_A},
	{"B Button",	RETRO_DEVICE_ID_JOYPAD_B,		BTN_ID_B},
	{"X Button",	RETRO_DEVICE_ID_JOYPAD_X,		BTN_ID_X},
	{"Y Button",	RETRO_DEVICE_ID_JOYPAD_Y,		BTN_ID_Y},
	{"Start",		RETRO_DEVICE_ID_JOYPAD_START,	BTN_ID_START},
	{"Select",		RETRO_DEVICE_ID_JOYPAD_SELECT,	BTN_ID_SELECT},
	{"L1 Button",	RETRO_DEVICE_ID_JOYPAD_L,		BTN_ID_L1},
	{"R1 Button",	RETRO_DEVICE_ID_JOYPAD_R,		BTN_ID_R1},
	{"L2 Button",	RETRO_DEVICE_ID_JOYPAD_L2,		BTN_ID_L2},
	{"R2 Button",	RETRO_DEVICE_ID_JOYPAD_R2,		BTN_ID_R2},
	{"L3 Button",	RETRO_DEVICE_ID_JOYPAD_L3,		BTN_ID_L3},
	{"R3 Button",	RETRO_DEVICE_ID_JOYPAD_R3,		BTN_ID_R3},
	{NULL,0,0}
};

ButtonMapping core_button_mapping[RETRO_BUTTON_COUNT+1] = {0};

static const char* device_button_names[LOCAL_BUTTON_COUNT] = {
	[BTN_ID_DPAD_UP]	= "UP",
	[BTN_ID_DPAD_DOWN]	= "DOWN",
	[BTN_ID_DPAD_LEFT]	= "LEFT",
	[BTN_ID_DPAD_RIGHT]	= "RIGHT",
	[BTN_ID_SELECT]		= "SELECT",
	[BTN_ID_START]		= "START",
	[BTN_ID_Y]			= "Y",
	[BTN_ID_X]			= "X",
	[BTN_ID_B]			= "B",
	[BTN_ID_A]			= "A",
	[BTN_ID_L1]			= "L1",
	[BTN_ID_R1]			= "R1",
	[BTN_ID_L2]			= "L2",
	[BTN_ID_R2]			= "R2",
	[BTN_ID_L3]			= "L3",
	[BTN_ID_R3]			= "R3",
};


// NOTE: these must be in BTN_ID_ order also off by 1 because of NONE (which is -1 in BTN_ID_ land)
char* button_labels[] = {
	"NONE", // displayed by default
	"UP",
	"DOWN",
	"LEFT",
	"RIGHT",
	"A",
	"B",
	"X",
	"Y",
	"START",
	"SELECT",
	"L1",
	"R1",
	"L2",
	"R2",
	"L3",
	"R3",
	"MENU+UP",
	"MENU+DOWN",
	"MENU+LEFT",
	"MENU+RIGHT",
	"MENU+A",
	"MENU+B",
	"MENU+X",
	"MENU+Y",
	"MENU+START",
	"MENU+SELECT",
	"MENU+L1",
	"MENU+R1",
	"MENU+L2",
	"MENU+R2",
	"MENU+L3",
	"MENU+R3",
	NULL,
};
static char* overclock_labels[] = {
	"Auto",
	"Performance",
	"Powersave",
	NULL,
};

// TODO: this should be provided by the core
char* gamepad_labels[] = {
	"Standard",
	"DualShock",
	NULL,
};
char* gamepad_values[] = {
	"1",
	"517",
	NULL,
};

// CONFIG_NONE, CONFIG_CONSOLE, CONFIG_GAME defined in minarch_internal.h

char* getScreenScalingDesc(void) {
	if (GFX_supportsOverscan()) {
		return "Native uses integer scaling. Aspect uses core nreported aspect ratio.\nAspect screen uses screen aspect ratio\n Fullscreen has non-square\npixels. Cropped is integer scaled then cropped.";
	}
	else {
		return "Native uses integer scaling.\nAspect uses core reported aspect ratio.\nAspect screen uses screen aspect ratio\nFullscreen has non-square pixels.";
	}
}
int getScreenScalingCount(void) {
	return GFX_supportsOverscan() ? 5 : 4;
}
	

struct Config config = {
	.frontend = { // (OptionList)
		.count = FE_OPT_COUNT,
		.options = (Option[]){
			[FE_OPT_SCALING] = {
				.key	= "minarch_screen_scaling", 
				.name	= "Screen Scaling",
				.desc	= NULL, // will call getScreenScalingDesc()
				.default_value = 1,
				.value = 1,
				.count = 3, // will call getScreenScalingCount()
				.values = scaling_labels,
				.labels = scaling_labels,
			},
			[FE_OPT_RESAMPLING] = {
				.key	= "minarch__resampling_quality", 
				.name	= "Audio Resampling Quality",
				.desc	= "Resampling quality higher takes more CPU",
				.default_value = 2,
				.value = 2,
				.count = 4,
				.values = resample_labels,
				.labels = resample_labels,
			},
			[FE_OPT_AMBIENT] = {
				.key	= "minarch_ambient", 
				.name	= "Ambient Mode",
				.desc	= "Makes your leds follow on screen colors",
				.default_value = 0,
				.value = 0,
				.count = 6,
				.values = ambient_labels,
				.labels = ambient_labels,
			},
			[FE_OPT_EFFECT] = {
				.key	= "minarch_screen_effect",
				.name	= "Screen Effect",
				.desc	= "Grid simulates an LCD grid.\nLine simulates CRT scanlines.\nEffects usually look best at native scaling.",
				.default_value = 0,
				.value = 0,
				.count = 3,
				.values = effect_labels,
				.labels = effect_labels,
			},
			[FE_OPT_OVERLAY] = {
				.key	= "minarch_overlay",
				.name	= "Overlay",
				.desc	= "Choose a custom overlay png from the Overlays folder",
				.default_value = 0,
				.value = 0,
				.count = 1,
				.values = overlay_labels,
				.labels = overlay_labels,
			},
			[FE_OPT_SCREENX] = {
				.key	= "minarch_screen_offsetx",
				.name	= "Offset screen X",
				.desc	= "Offset X pixels",
				.default_value = 64,
				.value = 64,
				.count = 129,
				.values = offset_labels,
				.labels = offset_labels,
			},
			[FE_OPT_SCREENY] = {
				.key	= "minarch_screen_offsety",
				.name	= "Offset screen Y",
				.desc	= "Offset Y pixels",
				.default_value = 64,
				.value = 64,
				.count = 129,
				.values = offset_labels,
				.labels = offset_labels,
			},
			[FE_OPT_SHARPNESS] = {
				// 	.key	= "minarch_screen_sharpness",
				.key	= "minarch_scale_filter",
				.name	= "Screen Sharpness",
				.desc	= "LINEAR smooths lines, but works better when final image is at higher resolution, so either core that outputs higher resolution or upscaling with shaders",
				.default_value = 1,
				.value = 1,
				.count = 3,
				.values = sharpness_labels,
				.labels = sharpness_labels,
			},
			[FE_OPT_SYNC_REFERENCE] = {
				.key	= "minarch_sync_reference",
				.name	= "Core Sync",
				.desc	= "Choose what should be used as a\nreference for the frame rate.\n\"Native\" uses the emulator frame rate,\n\"Screen\" uses the frame rate of the screen.",
				.default_value = SYNC_SRC_AUTO,
				.value = SYNC_SRC_AUTO,
				.count = 3,
				.values = sync_ref_labels,
				.labels = sync_ref_labels,
			},
			[FE_OPT_OVERCLOCK] = {
				.key	= "minarch_cpu_speed",
				.name	= "CPU Speed",
				.desc	= "Choose how the CPU scales.\nAuto is recommended for most users.",
				.default_value = 0,
				.value = 0,
				.count = 3,
				.values = overclock_labels,
				.labels = overclock_labels,
			},
			[FE_OPT_DEBUG] = {
				.key	= "minarch_debug_hud",
				.name	= "Debug HUD",
				.desc	= "Show frames per second, cpu load,\nresolution, and scaler information.",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_MAXFF] = {
				.key	= "minarch_max_ff_speed",
				.name	= "Max FF Speed",
				.desc	= "Fast forward will not exceed the\nselected speed (but may be less\ndepending on game and emulator).",
				.default_value = 3, // 4x
				.value = 3, // 4x
				.count = 8,
				.values = max_ff_labels,
				.labels = max_ff_labels,
			},
			[FE_OPT_FF_AUDIO] = {
				.key	= "minarch__ff_audio", 
				.name	= "Fast forward audio",
				.desc	= "Play or mute audio when fast forwarding.",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_REWIND_ENABLE] = {
				.key	= "minarch_rewind_enable",
				.name	= "Rewind",
				.desc	= "Enable in-memory rewind buffer.\nMust set a shortcut to access rewind during gameplay.\nUses extra CPU and memory.",
				.default_value = MINARCH_DEFAULT_REWIND_ENABLE ? 1 : 0,
				.value = MINARCH_DEFAULT_REWIND_ENABLE ? 1 : 0,
				.count = 2,
				.values = rewind_enable_labels,
				.labels = rewind_enable_labels,
			},
			[FE_OPT_REWIND_BUFFER] = {
				.key	= "minarch_rewind_buffer_mb",
				.name	= "Rewind Buffer (MB)",
				.desc	= "Memory reserved for rewind snapshots.\nIncrease for longer rewind times.",
				.default_value = 3, // 64MB
				.value = 3,
				.count = 6,
				.values = rewind_buffer_labels,
				.labels = rewind_buffer_labels,
			},
			[FE_OPT_REWIND_GRANULARITY] = {
				.key	= "minarch_rewind_granularity",
				.name	= "Rewind Interval",
				.desc	= "Interval between rewind snapshots.\nShorter intervals improve smoothness during rewind,\nbut increase CPU and memory usage.",
				.default_value = 0, // 16ms
				.value = 0,
				.count = 12,
				.values = rewind_granularity_values,
				.labels = rewind_granularity_labels,
			},
			[FE_OPT_REWIND_COMPRESSION] = {
				.key	= "minarch_rewind_compression",
				.name	= "Rewind Compression",
				.desc	= "Compress rewind snapshots to save memory at the cost of CPU.",
				.default_value = 1,
				.value = 1,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_REWIND_COMPRESSION_ACCEL] = {
				.key	= "minarch_rewind_compression_speed",
				.name	= "Rewind Compression Speed",
				.desc	= "LZ4 acceleration used for rewind snapshots.\nLower values compress more but use more CPU.",
				.default_value = 1, // value 2
				.value = 1,
				.count = 5,
				.values = rewind_compression_accel_values,
				.labels = rewind_compression_accel_labels,
			},
			[FE_OPT_REWIND_AUDIO] = {
				.key	= "minarch_rewind_audio",
				.name	= "Rewind audio",
				.desc	= "Play or mute audio when rewinding.",
				.default_value = MINARCH_DEFAULT_REWIND_AUDIO ? 1 : 0,
				.value = MINARCH_DEFAULT_REWIND_AUDIO ? 1 : 0,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_COUNT] = {NULL}
		}
	},
	.core = { // (OptionList)
		.count = 0,
		.options = (Option[]){
			{NULL},
		},
	},
	.shaders = { // (OptionList)
		.count = 18,
		.options = (Option[]){
			[SH_EXTRASETTINGS] = {
				.key	= "minarch_shaders_settings", 
				.name	= "Optional Shaders Settings",
				.desc	= "If shaders have extra settings they will show up in this settings menu",
				.default_value = 1,
				.value = 1,
				.count = 0,
				.values = NULL,
				.labels = NULL,
			},
			[SH_SHADERS_PRESET] = {
				.key	= "minarch_shaders_preset", 
				.name	= "Shader / Emulator Settings Preset",
				.desc	= "Load a premade shaders/emulators config.\nTo try out a preset, exit the game without saving settings!",
				.default_value = 1,
				.value = 1,
				.count = 0,
				.values = NULL,
				.labels = NULL,
			},
			[SH_NROFSHADERS] = {
				.key	= "minarch_nrofshaders", 
				.name	= "Number of Shaders",
				.desc	= "Number of shaders 1 to 3",
				.default_value = 0,
				.value = 0,
				.count = 4,
				.values = nrofshaders_labels,
				.labels = nrofshaders_labels,
			},
			
			[SH_SHADER1] = {
				.key	= "minarch_shader1", 
				.name	= "Shader 1",
				.desc	= "Shader 1 program to run",
				.default_value = 1,
				.value = 1,
				.count = 0,
				.values = NULL,
				.labels = NULL,
			},
			[SH_SHADER1_FILTER] = {
				.key	= "minarch_shader1_filter", 
				.name	= "Shader 1 Filter",
				.desc	= "Method of upscaling, NEAREST or LINEAR",
				.default_value = 1,
				.value = 1,
				.count = 2,
				.values = shfilter_labels,
				.labels = shfilter_labels,
			},
			[SH_SRCTYPE1] = {
				.key	= "minarch_shader1_srctype", 
				.name	= "Shader 1 Source type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_SCALETYPE1] = {
				.key	= "minarch_shader1_scaletype", 
				.name	= "Shader 1 Texture Type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 1,
				.value = 1,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_UPSCALE1] = {
				.key	= "minarch_shader1_upscale", 
				.name	= "Shader 1 Scale",
				.desc	= "This will scale images x times,\nscreen scales to screens resolution (can hit performance)",
				.default_value = 1,
				.value = 1,
				.count = 9,
				.values = shupscale_labels,
				.labels = shupscale_labels,
			},
			[SH_SHADER2] = {
				.key	= "minarch_shader2", 
				.name	= "Shader 2",
				.desc	= "Shader 2 program to run",
				.default_value = 0,
				.value = 0,
				.count = 0,
				.values = NULL,
				.labels = NULL,

			},
			[SH_SHADER2_FILTER] = {
				.key	= "minarch_shader2_filter", 
				.name	= "Shader 2 Filter",
				.desc	= "Method of upscaling, NEAREST or LINEAR",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = shfilter_labels,
				.labels = shfilter_labels,
			},
			[SH_SRCTYPE2] = {
				.key	= "minarch_shader2_srctype", 
				.name	= "Shader 2 Source type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_SCALETYPE2] = {
				.key	= "minarch_shader2_scaletype", 
				.name	= "Shader 2 Texture Type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 1,
				.value = 1,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_UPSCALE2] = {
				.key	= "minarch_shader2_upscale", 
				.name	= "Shader 2 Scale",
				.desc	= "This will scale images x times,\nscreen scales to screens resolution (can hit performance)",
				.default_value = 0,
				.value = 0,
				.count = 9,
				.values = shupscale_labels,
				.labels = shupscale_labels,
			},
			[SH_SHADER3] = {
				.key	= "minarch_shader3", 
				.name	= "Shader 3",
				.desc	= "Shader 3 program to run",
				.default_value = 2,
				.value = 2,
				.count = 0,
				.values = NULL,
				.labels = NULL,

			},
			[SH_SHADER3_FILTER] = {
				.key	= "minarch_shader3_filter", 
				.name	= "Shader 3 Filter",
				.desc	= "Method of upscaling, NEAREST or LINEAR",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = shfilter_labels,
				.labels = shfilter_labels,
			},
			[SH_SRCTYPE3] = {
				.key	= "minarch_shader3_srctype", 
				.name	= "Shader 3 Source type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_SCALETYPE3] = {
				.key	= "minarch_shader3_scaletype", 
				.name	= "Shader 3 Texture Type",
				.desc	= "This will choose resolution source to scale from",
				.default_value = 1,
				.value = 1,
				.count = 2,
				.values = shscaletype_labels,
				.labels = shscaletype_labels,
			},
			[SH_UPSCALE3] = {
				.key	= "minarch_shader3_upscale", 
				.name	= "Shader 3 Scale",
				.desc	= "This will scale images x times,\nscreen scales to screens resolution (can hit performance)",
				.default_value = 0,
				.value = 0,
				.count = 9,
				.values = shupscale_labels,
				.labels = shupscale_labels,
			},
			{NULL}
		},
	},
	.shaderpragmas = {{
		.count = 0,
		.options = NULL,
	}},
	.controls = default_button_mapping,
	.shortcuts = (ButtonMapping[]){
		[SHORTCUT_SAVE_STATE]			= {"Save State",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_LOAD_STATE]			= {"Load State",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_RESET_GAME]			= {"Reset Game",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_SAVE_QUIT]			= {"Save & Quit",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_CYCLE_SCALE]			= {"Cycle Scaling",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_CYCLE_EFFECT]			= {"Cycle Effect",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_FF]			= {"Toggle FF",			-1, BTN_ID_NONE, 0},
		[SHORTCUT_HOLD_FF]				= {"Hold FF",			-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_REWIND]		= {"Toggle Rewind",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_HOLD_REWIND]			= {"Hold Rewind",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_GAMESWITCHER]			= {"Game Switcher",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_SCREENSHOT]           = {"Screenshot",        -1, BTN_ID_NONE, 0},
		// Trimui only
		[SHORTCUT_TOGGLE_TURBO_A]		= {"Toggle Turbo A",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_B]		= {"Toggle Turbo B",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_X]		= {"Toggle Turbo X",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_Y]		= {"Toggle Turbo Y",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_L]		= {"Toggle Turbo L",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_L2]		= {"Toggle Turbo L2",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_R]		= {"Toggle Turbo R",	-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_TURBO_R2]		= {"Toggle Turbo R2",	-1, BTN_ID_NONE, 0},
		// -----
		{NULL}
	},
};


///////////////////////////////
static struct Special {
	int palette_updated;
} special;
void Special_updatedDMGPalette(int frames) {
	// LOG_info("Special_updatedDMGPalette(%i)\n", frames);
	special.palette_updated = frames; // must wait a few frames
}
static void Special_refreshDMGPalette(void) {
	special.palette_updated -= 1;
	if (special.palette_updated>0) return;
	
	int rgb = getInt("/tmp/dmg_grid_color");
	GFX_setEffectColor(rgb);
}
static void Special_init(void) {
	if (special.palette_updated>1) special.palette_updated = 1;
	// else if (exactMatch((char*)core.tag, "GBC"))  {
	// 	putInt("/tmp/dmg_grid_color",0xF79E);
	// 	special.palette_updated = 1;
	// }
}
void Special_render(void) {
	if (special.palette_updated) Special_refreshDMGPalette();
}
static void Special_quit(void) {
	system("rm -f /tmp/dmg_grid_color");
}
///////////////////////////////

///////////////////////////////

void hdmimon(void) {
	// handle HDMI change
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi==-1) had_hdmi = has_hdmi;
	if (has_hdmi!=had_hdmi) {
		had_hdmi = has_hdmi;

		LOG_info("restarting after HDMI change...\n");
		Menu_beforeSleep();
		sleep(4);
		show_menu = 0;
		quit = 1;
	}
}

static void chooseSyncRef(void) {
	switch (sync_ref) {
		case SYNC_SRC_AUTO:   use_core_fps = (core.get_region() == RETRO_REGION_PAL); break;
		case SYNC_SRC_SCREEN: use_core_fps = 0; break;
		case SYNC_SRC_CORE:   use_core_fps = 1; break;
	}
	LOG_info("%s: sync_ref is set to %s, game region is %s, use core fps = %s\n",
		  __FUNCTION__,
		  sync_ref_labels[sync_ref],
		  core.get_region() == RETRO_REGION_NTSC ? "NTSC" : "PAL",
		  use_core_fps ? "yes" : "no");
}

static void limitFF(void) {
	static uint64_t ff_frame_time = 0;
	static uint64_t last_time = 0;
	static int last_max_speed = -1;
	if (last_max_speed!=max_ff_speed) {
		last_max_speed = max_ff_speed;
		ff_frame_time = 1000000 / (core.fps * (max_ff_speed + 1));
	}
	
	uint64_t now = getMicroseconds();
	if (fast_forward && max_ff_speed) {
		if (last_time == 0) last_time = now;
		int elapsed = now - last_time;
		if (elapsed>0 && elapsed<0x80000) {
			if (elapsed<ff_frame_time) {
				int delay = (ff_frame_time - elapsed) / 1000;
				if (delay>0 && delay<17) { // don't allow a delay any greater than a frame
					SDL_Delay(delay);
				}
			}
			last_time += ff_frame_time;
			return;
		}
	}
	last_time = now;
}

static void run_frame(void) {
	// if rewind is toggled, fast-forward toggle must stay off; fast-forward hold pauses rewind
	int do_rewind = (rewind_pressed || rewind_toggle) && !(rewind_toggle && ff_hold_active);
	if (do_rewind) {
		int was_rewinding = rewinding;
		int rewind_result = Rewind_step_back();
		if (rewind_result == REWIND_STEP_OK) {
			// Actually stepped back - run one frame to render the restored state
			rewinding = 1;
			fast_forward = 0;
			core.run();
		}
		else if (rewind_result == REWIND_STEP_CADENCE) {
			// Waiting for cadence - don't run core, just re-render current frame
			rewinding = 1;
			fast_forward = 0;
			// Poll input manually since core.run() isn't called
			input_poll_callback();
			// Skip core.run() entirely to avoid advancing the game
		}
		else {
			int hold_empty = rewind_ctx.enabled && rewind_pressed && !rewind_toggle;
			if (hold_empty) {
				// Hold-to-rewind: freeze when empty to avoid advance/rewind oscillation.
				rewinding = was_rewinding ? 1 : 0;
				// Poll input manually so release is detected while core.run() is skipped
				input_poll_callback();
			} else {
				// Buffer empty: auto untoggle rewind, resume FF if it was paused for a hold
				if (rewind_toggle) rewind_toggle = 0;
				if (ff_paused_by_rewind_hold && ff_toggled) {
					ff_paused_by_rewind_hold = 0;
					fast_forward = setFastForward(1);
				}
				if (was_rewinding) {
					rewinding = 1;
					Rewind_sync_encode_state();
				}
				rewinding = 0;
				core.run();
				Rewind_push(0);
			}
		}
	}
	else {
		Rewind_sync_encode_state();
		rewinding = 0;
		if (ff_paused_by_rewind_hold && !rewind_pressed) {
			// resume fast forward after hold rewind ends
			if (ff_toggled) fast_forward = setFastForward(1);
			ff_paused_by_rewind_hold = 0;
		}

		core.run();
		Rewind_push(0);
	}
	limitFF();
}

#define PWR_UPDATE_FREQ 5
#define PWR_UPDATE_FREQ_INGAME 20

// We need to do this on the audio thread (aka main thread currently)
static bool resetAudio = false;

void onAudioSinkChanged(int device, int watch_event)
{
	switch (watch_event)
	{
	case DIRWATCH_CREATE: LOG_info("callback reason: DIRWATCH_CREATE\n"); break;
	case DIRWATCH_DELETE: LOG_info("callback reason: DIRWATCH_DELETE\n"); break;
	case FILEWATCH_MODIFY: LOG_info("callback reason: FILEWATCH_MODIFY\n"); break;
	case FILEWATCH_DELETE: LOG_info("callback reason: FILEWATCH_DELETE\n"); break;
	case FILEWATCH_CLOSE_WRITE: LOG_info("callback reason: FILEWATCH_CLOSE_WRITE\n"); break;
	}

	resetAudio = true;

	// FIXME: This shouldnt be necessary, alsa should just read .asoundrc for the changed defult device.
	if(device == AUDIO_SINK_BLUETOOTH)
		SDL_setenv("AUDIODEV", "bluealsa", 1);
	else
		SDL_setenv("AUDIODEV", "default", 1);

	//if(device != AUDIO_SINK_DEFAULT && !exists(SDCARD_PATH "/.userdata/tg5040/.asoundrc"))
	//	LOG_error("asoundrc is not there yet!!!\n");
	//else if(device == AUDIO_SINK_DEFAULT && exists(SDCARD_PATH "/.userdata/tg5040/.asoundrc"))
	//	LOG_error("asoundrc is not deleted yet!!!\n");
}

int main(int argc , char* argv[]) {
	//static char asoundpath[MAX_PATH];
	//sprintf(asoundpath, "%s/.asoundrc", getenv("HOME"));
	//LOG_info("minarch: need asoundrc at %s\n", asoundpath);
	//if(exists(asoundpath))
	//	LOG_info("asoundrc exists at %s\n", asoundpath);
	//else 
	//	LOG_info("asoundrc does not exist at %s\n", asoundpath);

	if(argc < 2)
		return EXIT_FAILURE;

	setOverclock(1); // start up in performance mode, faster init
	PWR_pinToCores(CPU_CORE_PERFORMANCE); // thread affinity

	char core_path[MAX_PATH];
	char rom_path[MAX_PATH];
	char tag_name[MAX_PATH];

	strcpy(core_path, argv[1]);
	strcpy(rom_path, argv[2]);
	getEmuName(rom_path, tag_name);
	
	LOG_info("rom_path: %s\n", rom_path);
	
	screen = GFX_init(MODE_MENU);

	// initialize default shaders
	GFX_initShaders();
	PLAT_initNotificationTexture();

	PAD_init();
	DEVICE_WIDTH = screen->w;
	DEVICE_HEIGHT = screen->h;
	DEVICE_PITCH = screen->pitch;
	// LOG_info("DEVICE_SIZE: %ix%i (%i)\n", DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);
	
	LEDS_initLeds();
	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON)
		PWR_disableSleep();
	MSG_init();
	IMG_Init(IMG_INIT_PNG);
	Core_open(core_path, tag_name);

	Game_open(rom_path); // nes tries to load gamegenie setting before this returns ffs
	if (!game.is_open) goto finish;
	
	simple_mode = exists(SIMPLE_MODE_PATH);
	
	// restore options
	Config_load(); // before init?
	Config_init();
	Config_readOptions(); // cores with boot logo option (eg. gb) need to load options early
	setOverclock(overclock); // why twice?
	
	Core_init();

	// Initialize RetroAchievements after core.init() but before Core_load()
	// Set up memory accessors for achievement memory reading
	RA_setMemoryAccessors(core.get_memory_data, core.get_memory_size);
	RA_init();

	// TODO: find a better place to do this
	// mixing static and loaded data is messy
	// why not move to Core_init()?
	Menu_setCoreVersionDesc(core.version);
	Core_load();
	
	Input_init(NULL);
	Config_readOptions(); // but others load and report options later (eg. nes)
	Config_readControls(); // restore controls (after the core has reported its defaults)

	// Mute audio during startup to avoid pops (InitSettings would be logical, but too late)
	SND_overrideMute(1);
	SND_init(core.sample_rate, core.fps);
	SND_registerDeviceWatcher(onAudioSinkChanged);
	InitSettings(); // after we initialize audio
	Menu_init();
	Notification_init();
	
	// Load game for RetroAchievements tracking (must be after Notification_init)
	// Pass ROM data if available, otherwise just path (for cores that load from file)
	{
		char* rom_path_for_ra = game.tmp_path[0] ? game.tmp_path : game.path;
		RA_loadGame(rom_path_for_ra, game.data, game.size, core.tag);
	}
	
	State_resume();
	Menu_initState(); // make ready for state shortcuts

	PWR_disableAutosleep();
	// we dont need five second updates while ingame, and wifi status isnt displayed either
	PWR_updateFrequency(PWR_UPDATE_FREQ, 0); 

	// force a vsync immediately before loop
	// for better frame pacing?
	GFX_clearAll();
	GFX_clearLayers(0);
	GFX_clear(screen);

	// need to draw real black background first otherwise u get weird pixels sometimes

	GFX_flip(screen);

	Special_init(); // after config

	chooseSyncRef();
	
	int has_pending_opt_change = 0;

	// then initialize custom  shaders from settings
	initShaders();
	Config_readOptions();
	applyShaderSettings();
	Rewind_init(core.serialize_size ? core.serialize_size() : 0);
	if (core.serialize_size) Rewind_on_state_change();
	// release config when all is loaded
	Config_free();

	LOG_info("total startup time %ims\n\n",SDL_GetTicks());
	while (!quit) {
		GFX_startFrame();

		run_frame();
		
		// Process RetroAchievements for this frame
		RA_doFrame();
		
		// Update and render notifications overlay
		Notification_update(SDL_GetTicks());
		
		// Poll for volume/brightness/colortemp changes and show system indicators
		{
			static int last_volume = -1;
			static int last_brightness = -1;
			static int last_colortemp = -1;
			
			int cur_volume = GetVolume();
			int cur_brightness = GetBrightness();
			int cur_colortemp = GetColortemp();
			
			if (last_volume == -1) {
				// First frame - just initialize cached values, don't show indicator
				last_volume = cur_volume;
				last_brightness = cur_brightness;
				last_colortemp = cur_colortemp;
			} else {
				// Check for changes
				if (cur_volume != last_volume) {
					last_volume = cur_volume;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_VOLUME);
				}
				if (cur_brightness != last_brightness) {
					last_brightness = cur_brightness;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_BRIGHTNESS);
				}
				if (cur_colortemp != last_colortemp) {
					last_colortemp = cur_colortemp;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_COLORTEMP);
				}
			}
		}
		
		Notification_renderToLayer(5);  // Always call - handles cleanup when inactive

		if (has_pending_opt_change) {
			has_pending_opt_change = 0;
			if (Core_updateAVInfo()) {
				LOG_info("AV info changed, reset sound system");
				SND_resetAudio(core.sample_rate, core.fps);
			}
			chooseSyncRef();
		}

		if (show_menu) {
			PWR_updateFrequency(PWR_UPDATE_FREQ,1);
			Menu_loop();
			// Process RA async operations while menu is shown
			RA_idle();
			PWR_updateFrequency(PWR_UPDATE_FREQ_INGAME,0);
			has_pending_opt_change = config.core.changed;
			chooseSyncRef();
		}

		if (resetAudio) {
			resetAudio = false;
			LOG_info("Resetting audio device config! (new state: %s)\n", SDL_getenv("AUDIODEV"));
			SND_resetAudio(core.sample_rate, core.fps);
		}

		hdmimon();
	}
	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);
	
	renderer.dst = pixels;
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, cw, ch, 32, cw * 4, SDL_PIXELFORMAT_ABGR8888
	);
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(rawSurface, screen->format->format, 0);
	screen = converted;
	SDL_FreeSurface(rawSurface);
	free(pixels); 
	GFX_animateSurfaceOpacity(converted, 0, 0, cw, ch, 255, 0, CFG_getMenuTransitions() ? 200 : 20, 1);
	SDL_FreeSurface(converted); 
	
	Video_cleanup();

	PLAT_clearTurbo();

	Menu_quit();
	QuitSettings();

finish:
    Perf_setCPUMonitorEnabled(0);

	// Unload game and shutdown RetroAchievements before Notification_quit —
	// RA background threads (sync, badge downloads) may call notification
	// APIs, so the notification mutex should outlive all RA threads.
	RA_unloadGame();
	RA_quit();
	Notification_quit();
	
	Game_close();
	Rewind_free();
	Core_unload();
	Core_quit();
	Core_close();
	Config_quit();
	Special_quit();
	MSG_quit();
	PWR_quit();
	VIB_quit();
	SND_removeDeviceWatcher();
	// Disabling this is a dumb hack for bluetooth, we should really be using 
	// bluealsa with --keep-alive=-1 - but SDL wont reconnect the stream on next start.
	// Reenable as soon as we have a more recent SDL available, if ever.
	//SND_quit();
	PAD_quit();
	GFX_quit();
	Menu_waitScreenshot();
	return EXIT_SUCCESS;
}

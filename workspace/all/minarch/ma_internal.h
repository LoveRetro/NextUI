#pragma once

#include <SDL2/SDL.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

#include "libretro.h"
#include "defines.h"
#include "api.h"
#include "config.h"
#include "utils.h"

struct Core {
	int initialized;
	int need_fullpath;

	const char tag[8]; // eg. GBC
	const char name[128]; // eg. gambatte
	const char version[128]; // eg. Gambatte (v0.5.0-netlink 7e02df6)
	const char extensions[128]; // eg. gb|gbc|dmg

	const char config_dir[MAX_PATH]; // eg. /mnt/sdcard/.userdata/rg35xx/GB-gambatte
	const char states_dir[MAX_PATH]; // eg. /mnt/sdcard/.userdata/arm-480/GB-gambatte
	const char saves_dir[MAX_PATH]; // eg. /mnt/sdcard/Saves/GB
	const char bios_dir[MAX_PATH]; // eg. /mnt/sdcard/Bios/GB
	const char cheats_dir[MAX_PATH]; // eg. /mnt/sdcard/Cheats/GB
	const char overlays_dir[MAX_PATH]; // eg. /mnt/sdcard/Cheats/GB

	double fps;
	double sample_rate;
	double aspect_ratio;

	void* handle;
	void (*init)(void);
	void (*deinit)(void);

	void (*get_system_info)(struct retro_system_info *info);
	void (*get_system_av_info)(struct retro_system_av_info *info);
	void (*set_controller_port_device)(unsigned port, unsigned device);

	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *data, size_t size);
	bool (*unserialize)(const void *data, size_t size);
	void (*cheat_reset)(void);
	void (*cheat_set)(unsigned id, bool enabled, const char*);
	bool (*load_game)(const struct retro_game_info *game);
	bool (*load_game_special)(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*unload_game)(void);
	unsigned (*get_region)(void);
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);

	retro_core_options_update_display_callback_t update_visibility_callback;
};

struct Game {
	char path[MAX_PATH];
	char name[MAX_PATH]; // TODO: rename to basename?
	char alt_name[MAX_PATH]; // alternate name, eg. unzipped rom file name
	char m3u_path[MAX_PATH];
	char tmp_path[MAX_PATH]; // location of unzipped file
	void* data;
	size_t size;
	int is_open;
};

enum {
	SCALE_NATIVE,
	SCALE_ASPECT,
	SCALE_ASPECT_SCREEN,
	SCALE_FULLSCREEN,
	SCALE_CROPPED,
	SCALE_COUNT,
};

extern enum retro_pixel_format fmt;

extern struct Core core;
extern struct Game game;
extern struct retro_disk_control_ext_callback disk_control_ext;
extern GFX_Renderer renderer;
extern SDL_Surface *screen;

extern int quit;
extern int show_menu;
extern int newScreenshot;
extern int fast_forward;
extern int rewinding;
extern int ff_audio;
extern int use_core_fps;
extern int rewind_pressed;
extern int rewind_toggle;
extern int ff_toggled;
extern int ff_hold_active;
extern int ff_paused_by_rewind_hold;

extern int screen_scaling;
extern int screen_effect;

extern int DEVICE_WIDTH;
extern int DEVICE_HEIGHT;
extern int DEVICE_PITCH;
extern int ambient_mode;
extern int show_debug;
extern int shader_reset_suppressed;

extern char* scaling_labels[];
extern int simple_mode;
extern int resampling_quality;
extern int screen_sharpness;
extern int cfg_screenx;
extern int cfg_screeny;
extern int overlay;
extern int sync_ref;
extern int max_ff_speed;
extern int overclock;

extern int rewind_cfg_enable;
extern int rewind_cfg_buffer_mb;
extern int rewind_cfg_granularity;
extern int rewind_cfg_audio;
extern int rewind_cfg_compress;
extern int rewind_cfg_lz4_acceleration;
extern int rewind_init_ready;

#include "ma_rewind.h"

/* -----------------------------------------------------------------------
   Input / shortcut / options enums and types
   Moved from minarch.c so the input module can access them.
   ----------------------------------------------------------------------- */

#define LOCAL_BUTTON_COUNT 16 // depends on device
#define RETRO_BUTTON_COUNT 16 // allow L3/R3 to be remapped by user if desired, eg. Virtual Boy uses extra buttons for right d-pad

typedef struct ButtonMapping {
	char* name;
	int retro;
	int local; // TODO: dislike this name...
	int mod;
	int default_;
	int ignore;
} ButtonMapping;

typedef struct Option {
	char* key;
	char* name; // desc
	char* desc; // info, truncated
	char* full; // info, longer but possibly still truncated
	char *category;
	char* var;
	int default_value;
	int value;
	int count; // TODO: drop this?
	int lock;
	int hidden;
	char** values;
	char** labels;
} Option;
typedef struct OptionCategory {
	char *key;
	char *desc;
	char *info;
} OptionCategory;
typedef struct OptionList {
	int count;
	int changed;
	Option* options;

	int enabled_count;
	Option** enabled_options;

	OptionCategory *categories;
} OptionList;

struct Config {
	char* system_cfg;
	char* default_cfg;
	char* user_cfg;
	char* shaders_preset;
	char* device_tag;
	OptionList frontend;
	OptionList core;
	OptionList shaders;
	OptionList shaderpragmas[3];
	ButtonMapping* controls;
	ButtonMapping* shortcuts;
	int loaded;
	int initialized;
};

extern struct Config config;
extern ButtonMapping default_button_mapping[];
extern ButtonMapping core_button_mapping[];
extern int gamepad_type;
extern int last_rewind_pressed;

void Config_syncFrontend(char* key, int value);

// Special per-game hooks (defined in minarch.c)
void Special_updatedDMGPalette(int frames);
void Special_render(void);

// Libretro environment callback (defined in minarch.c)
bool environment_callback(unsigned cmd, void *data);

enum {
	FE_OPT_SCALING,
	FE_OPT_RESAMPLING,
	FE_OPT_AMBIENT,
	FE_OPT_EFFECT,
	FE_OPT_OVERLAY,
	FE_OPT_SCREENX,
	FE_OPT_SCREENY,
	FE_OPT_SHARPNESS,
	FE_OPT_SYNC_REFERENCE,
	FE_OPT_OVERCLOCK,
	FE_OPT_DEBUG,
	FE_OPT_MAXFF,
	FE_OPT_FF_AUDIO,
	FE_OPT_REWIND_ENABLE,
	FE_OPT_REWIND_BUFFER,
	FE_OPT_REWIND_GRANULARITY,
	FE_OPT_REWIND_COMPRESSION,
	FE_OPT_REWIND_COMPRESSION_ACCEL,
	FE_OPT_REWIND_AUDIO,
	FE_OPT_COUNT,
};

enum {
	SHORTCUT_SAVE_STATE,
	SHORTCUT_LOAD_STATE,
	SHORTCUT_RESET_GAME,
	SHORTCUT_SAVE_QUIT,
	SHORTCUT_CYCLE_SCALE,
	SHORTCUT_CYCLE_EFFECT,
	SHORTCUT_TOGGLE_FF,
	SHORTCUT_HOLD_FF,
	SHORTCUT_TOGGLE_REWIND,
	SHORTCUT_HOLD_REWIND,
	SHORTCUT_GAMESWITCHER,
	SHORTCUT_SCREENSHOT,
	// Trimui only
	SHORTCUT_TOGGLE_TURBO_A,
	SHORTCUT_TOGGLE_TURBO_B,
	SHORTCUT_TOGGLE_TURBO_X,
	SHORTCUT_TOGGLE_TURBO_Y,
	SHORTCUT_TOGGLE_TURBO_L,
	SHORTCUT_TOGGLE_TURBO_L2,
	SHORTCUT_TOGGLE_TURBO_R,
	SHORTCUT_TOGGLE_TURBO_R2,
	//
	SHORTCUT_COUNT,
};

#include "ma_options.h"

/* -----------------------------------------------------------------------
   Symbols defined in minarch.c shared with extracted modules
   ----------------------------------------------------------------------- */

struct Cheats; // forward declaration for Core_applyCheats

// Label arrays
extern char* button_labels[];
extern char* gamepad_labels[];
extern char* gamepad_values[];
extern char* onoff_labels[];
extern char* sync_ref_labels[];
extern int has_custom_controllers;

// Shader option indices
enum {
	SH_EXTRASETTINGS,
	SH_SHADERS_PRESET,
	SH_NROFSHADERS,

	SH_SHADER1,
	SH_SHADER1_FILTER,
	SH_SRCTYPE1,
	SH_SCALETYPE1,
	SH_UPSCALE1,

	SH_SHADER2,
	SH_SHADER2_FILTER,
	SH_SRCTYPE2,
	SH_SCALETYPE2,
	SH_UPSCALE2,

	SH_SHADER3,
	SH_SHADER3_FILTER,
	SH_SRCTYPE3,
	SH_SCALETYPE3,
	SH_UPSCALE3,

	SH_NONE
};

// Sync reference source
enum {
	SYNC_SRC_AUTO,
	SYNC_SRC_SCREEN,
	SYNC_SRC_CORE,
};

// Config loaded state
enum {
	CONFIG_NONE,
	CONFIG_CONSOLE,
	CONFIG_GAME,
};

// Config write mode
enum {
	CONFIG_WRITE_ALL,
	CONFIG_WRITE_GAME,
};

// Functions defined in minarch.c
void hdmimon(void);
void Core_applyCheats(struct Cheats *cheats);

#include "ma_config.h"
#include "ma_menu.h"

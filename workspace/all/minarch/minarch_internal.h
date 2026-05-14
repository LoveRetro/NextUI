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

extern struct Core core;
extern struct Game game;
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

extern int rewind_cfg_enable;
extern int rewind_cfg_buffer_mb;
extern int rewind_cfg_granularity;
extern int rewind_cfg_audio;
extern int rewind_cfg_compress;
extern int rewind_cfg_lz4_acceleration;

bool getAlias(char* path, char* alias);

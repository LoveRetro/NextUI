#include <dlfcn.h>
#include <libgen.h>
#include <string.h>

#include "minarch_internal.h"
#include "minarch_saves.h"
#include "minarch_video.h"
#include "minarch_audio.h"
#include "minarch_input.h"
#include "minarch_cheats.h"
#include "minarch_core.h"


void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');
	tmp[0] = '\0';
}
void Core_open(const char* core_path, const char* tag_name) {
	LOG_info("Core_open\n");
	core.handle = dlopen(core_path, RTLD_LAZY);
	
	if (!core.handle) LOG_error("%s\n", dlerror());
	
	core.init = dlsym(core.handle, "retro_init");
	core.deinit = dlsym(core.handle, "retro_deinit");
	core.get_system_info = dlsym(core.handle, "retro_get_system_info");
	core.get_system_av_info = dlsym(core.handle, "retro_get_system_av_info");
	core.set_controller_port_device = dlsym(core.handle, "retro_set_controller_port_device");
	core.reset = dlsym(core.handle, "retro_reset");
	core.run = dlsym(core.handle, "retro_run");
	core.serialize_size = dlsym(core.handle, "retro_serialize_size");
	core.serialize = dlsym(core.handle, "retro_serialize");
	core.unserialize = dlsym(core.handle, "retro_unserialize");
	core.cheat_reset = dlsym(core.handle, "retro_cheat_reset");
	core.cheat_set = dlsym(core.handle, "retro_cheat_set");
	core.load_game = dlsym(core.handle, "retro_load_game");
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");
	core.unload_game = dlsym(core.handle, "retro_unload_game");
	core.get_region = dlsym(core.handle, "retro_get_region");
	core.get_memory_data = dlsym(core.handle, "retro_get_memory_data");
	core.get_memory_size = dlsym(core.handle, "retro_get_memory_size");
	
	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);
	
	set_environment_callback = dlsym(core.handle, "retro_set_environment");
	set_video_refresh_callback = dlsym(core.handle, "retro_set_video_refresh");
	set_audio_sample_callback = dlsym(core.handle, "retro_set_audio_sample");
	set_audio_sample_batch_callback = dlsym(core.handle, "retro_set_audio_sample_batch");
	set_input_poll_callback = dlsym(core.handle, "retro_set_input_poll");
	set_input_state_callback = dlsym(core.handle, "retro_set_input_state");
	
	struct retro_system_info info = {};
	core.get_system_info(&info);
	

	LOG_info("Block Extract: %d\n", info.block_extract);

	Core_getName((char*)core_path, (char*)core.name);
	sprintf((char*)core.version, "%s (%s)", info.library_name, info.library_version);
	strcpy((char*)core.tag, tag_name);
	strcpy((char*)core.extensions, info.valid_extensions);
	
	core.need_fullpath = info.need_fullpath;
	
	LOG_info("core: %s version: %s tag: %s (valid_extensions: %s need_fullpath: %i)\n", core.name, core.version, core.tag, info.valid_extensions, info.need_fullpath);
	
	sprintf((char*)core.config_dir, USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.states_dir, SHARED_USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.saves_dir, SDCARD_PATH "/Saves/%s", core.tag);
	sprintf((char*)core.bios_dir, SDCARD_PATH "/Bios/%s", core.tag);
	sprintf((char*)core.cheats_dir, SDCARD_PATH "/Cheats/%s", core.tag);
	sprintf((char*)core.overlays_dir, SDCARD_PATH "/Overlays/%s", core.tag);
	
	char cmd[512];
	sprintf(cmd, "mkdir -p \"%s\"; mkdir -p \"%s\"", core.config_dir, core.states_dir);
	system(cmd);

	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(input_poll_callback);
	set_input_state_callback(input_state_callback);
}
void Core_init(void) {
	LOG_info("Core_init\n");
	core.init();
	core.initialized = 1;
}

void Core_applyCheats(struct Cheats *cheats)
{
	if (!cheats)
		return;

	if (!core.cheat_reset || !core.cheat_set)
		return;

	core.cheat_reset();
	for (int i = 0; i < cheats->count; i++) {
		if (cheats->cheats[i].enabled) {
			core.cheat_set(i, cheats->cheats[i].enabled, cheats->cheats[i].code);
		}
	}
}

int Core_updateAVInfo(void) {
	struct retro_system_av_info av_info = {};
	core.get_system_av_info(&av_info);

	double a = av_info.geometry.aspect_ratio;
	if (a<=0) a = (double)av_info.geometry.base_width / av_info.geometry.base_height;

	int changed = (core.fps != av_info.timing.fps || core.sample_rate != av_info.timing.sample_rate || core.aspect_ratio != a);

	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;
	core.aspect_ratio = a;

	if (changed) LOG_info("aspect_ratio: %f (%ix%i) fps: %f\n", a, av_info.geometry.base_width,av_info.geometry.base_height, core.fps);

	return changed;
}

void Core_load(void) {
	LOG_info("Core_load\n");
	struct retro_game_info game_info;
	game_info.path = game.tmp_path[0]?game.tmp_path:game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	LOG_info("game path: %s (%i)\n", game_info.path, game.size);
	core.load_game(&game_info);

	if (Cheats_load())
		Core_applyCheats(&cheatcodes);

	SRAM_read();
	RTC_read();
	// NOTE: must be called after core.load_game!
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD); // set a default, may update after loading configs
	Core_updateAVInfo();
}
void Core_reset(void) {
	core.reset();
	Rewind_on_state_change();
}
void Core_unload(void) {
	// Disabling this is a dumb hack for bluetooth, we should really be using 
	// bluealsa with --keep-alive=-1 - but SDL wont reconnect the stream on next start.
	// Reenable as soon as we have a more recent SDL available, if ever.
	//SND_quit();
}
void Core_quit(void) {
	if (core.initialized) {
		SRAM_write();
		Cheats_free();
		RTC_write();
		core.unload_game();
		core.deinit();
		core.initialized = 0;
	}
}
void Core_close(void) {
	if (core.handle) dlclose(core.handle);
}

#include <stdlib.h>
#include <msettings.h>

#include <SDL2/SDL_image.h>

#include "notification.h"
#include "ra_integration.h"

#include "ma_internal.h"
#include "ma_cheats.h"
#include "ma_audio.h"
#include "ma_input.h"
#include "ma_options.h"
#include "ma_frontend_opts.h"
#include "ma_saves.h"
#include "ma_video.h"
#include "ma_core.h"
#include "ma_game.h"
#include "ma_environment.h"
#include "ma_config.h"
#include "ma_runframe.h"

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

#define PWR_UPDATE_FREQ 5
#define PWR_UPDATE_FREQ_INGAME 20

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
	SND_registerDeviceWatcher(Audio_onSinkChanged);
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

		Audio_checkAndResetIfNeeded();

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

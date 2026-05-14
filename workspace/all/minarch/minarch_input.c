#include "minarch_internal.h"
#include "minarch_input.h"

#include <string.h>

int setFastForward(int enable) {
	int val = enable ? 1 : 0;
	if (fast_forward != val) {
		LOG_info("FF state -> %i\n", val);
	}
	fast_forward = val;
	return val;
}

static uint32_t buttons = 0; // RETRO_DEVICE_ID_JOYPAD_* buttons
static int ignore_menu = 0;
void input_poll_callback(void) {
	PAD_poll();

	int show_setting = 0;
	PWR_update(NULL, &show_setting, Menu_beforeSleep, Menu_afterSleep);

	// I _think_ this can stay as is...
	if (PAD_justPressed(BTN_MENU)) {
		ignore_menu = 0;
	}
	if (PAD_isPressed(BTN_MENU) && (PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS))) {
		ignore_menu = 1;
	}
	if (PAD_isPressed(BTN_MENU) && PAD_isPressed(BTN_SELECT)) {
		ignore_menu = 1;
		newScreenshot = 1;
		quit = 1;
		Menu_saveState();
		putFile(GAME_SWITCHER_PERSIST_PATH, game.path + strlen(SDCARD_PATH));
		GFX_clear(screen);

	}

	if (PAD_justPressed(BTN_POWER)) {

	}
	else if (PAD_justReleased(BTN_POWER)) {

	}

	static int toggled_ff_on = 0; // this logic only works because TOGGLE_FF is before HOLD_FF in the menu...
	rewind_pressed = 0;
	for (int i=0; i<SHORTCUT_COUNT; i++) {
		ButtonMapping* mapping = &config.shortcuts[i];
		int btn = 1 << mapping->local;
		if (btn==BTN_NONE) continue; // not bound
		if (!mapping->mod || PAD_isPressed(BTN_MENU)) {
			if (i==SHORTCUT_TOGGLE_FF) {
				if (PAD_justPressed(btn)) {
					toggled_ff_on = setFastForward(!fast_forward);
					ff_toggled = toggled_ff_on;
					ff_hold_active = 0;
					if (ff_toggled && rewind_toggle) {
						// last toggle wins: disable rewind toggle when FF toggle is enabled
						rewind_toggle = 0;
						rewind_pressed = 0;
						Rewind_sync_encode_state();
						rewinding = 0;
					}
					if (mapping->mod) ignore_menu = 1;
					break;
				}
				else if (PAD_justReleased(btn)) {
					if (mapping->mod) ignore_menu = 1;
					break;
				}
			}
			else if (i==SHORTCUT_HOLD_FF) {
				// don't allow turn off fast_forward with a release of the hold button
				// if it was initially turned on with the toggle button
				if (PAD_justPressed(btn) || (!toggled_ff_on && PAD_justReleased(btn))) {
					int pressed = PAD_isPressed(btn);
					fast_forward = setFastForward(pressed);
					ff_hold_active = pressed ? 1 : 0;
					if (mapping->mod) ignore_menu = 1; // very unlikely but just in case
				}
				if (PAD_justReleased(btn) && toggled_ff_on) {
					ff_hold_active = 0;
				}
			}
			else if (i==SHORTCUT_HOLD_REWIND) {
				rewind_pressed = PAD_isPressed(btn) ? 1 : 0;
				if (rewind_pressed != last_rewind_pressed) {
					LOG_info("Rewind hotkey %s\n", rewind_pressed ? "pressed" : "released");
					last_rewind_pressed = rewind_pressed;
				}
				if (rewind_pressed && ff_toggled && !ff_paused_by_rewind_hold) {
					ff_paused_by_rewind_hold = 1;
					fast_forward = setFastForward(0);
				}
				else if (!rewind_pressed && ff_paused_by_rewind_hold) {
					ff_paused_by_rewind_hold = 0;
					if (ff_toggled) fast_forward = setFastForward(1);
				}
				if (mapping->mod && rewind_pressed) ignore_menu = 1;
			}
			else if (i==SHORTCUT_TOGGLE_REWIND) {
				if (PAD_justPressed(btn)) {
					rewind_toggle = !rewind_toggle;
					if (rewind_toggle && ff_toggled) {
						// disable fast forward toggle when rewinding is toggled on
						ff_toggled = 0;
						fast_forward = setFastForward(0);
						ff_paused_by_rewind_hold = 0;
					}
					if (mapping->mod) ignore_menu = 1;
					break;
				}
				else if (PAD_justReleased(btn)) {
					if (mapping->mod) ignore_menu = 1;
					break;
				}
			}
			// Trimui only
			else if (PLAT_canTurbo() && i>=SHORTCUT_TOGGLE_TURBO_A && i<=SHORTCUT_TOGGLE_TURBO_R2) {
				if (PAD_justPressed(btn)) {
					switch(i) {
						case SHORTCUT_TOGGLE_TURBO_A:  PLAT_toggleTurbo(BTN_ID_A); break;
						case SHORTCUT_TOGGLE_TURBO_B:  PLAT_toggleTurbo(BTN_ID_B); break;
						case SHORTCUT_TOGGLE_TURBO_X:  PLAT_toggleTurbo(BTN_ID_X); break;
						case SHORTCUT_TOGGLE_TURBO_Y:  PLAT_toggleTurbo(BTN_ID_Y); break;
						case SHORTCUT_TOGGLE_TURBO_L:  PLAT_toggleTurbo(BTN_ID_L1); break;
						case SHORTCUT_TOGGLE_TURBO_L2: PLAT_toggleTurbo(BTN_ID_L2); break;
						case SHORTCUT_TOGGLE_TURBO_R:  PLAT_toggleTurbo(BTN_ID_R1); break;
						case SHORTCUT_TOGGLE_TURBO_R2: PLAT_toggleTurbo(BTN_ID_R2); break;
						default: break;
					}
					break;
				}
				else if (PAD_justReleased(btn)) {
					break;
				}
			}
			else if (PAD_justPressed(btn)) {
				switch (i) {
					case SHORTCUT_SAVE_STATE:
						newScreenshot = 1;
						Menu_saveState();
						break;
					case SHORTCUT_LOAD_STATE: Menu_loadState(); break;
					case SHORTCUT_SCREENSHOT:
						Menu_screenshot();
						break;
					case SHORTCUT_RESET_GAME: core.reset(); break;
					case SHORTCUT_SAVE_QUIT:
						newScreenshot = 1;
						quit = 1;
						Menu_saveState();
						break;
					case SHORTCUT_GAMESWITCHER:
						newScreenshot = 1;
						quit = 1;
						Menu_saveState();
						putFile(GAME_SWITCHER_PERSIST_PATH, game.path + strlen(SDCARD_PATH));
						break;
					case SHORTCUT_CYCLE_SCALE:
						screen_scaling = (screen_scaling + 1) % config.frontend.options[FE_OPT_SCALING].count;
						Config_syncFrontend(config.frontend.options[FE_OPT_SCALING].key, screen_scaling);
						break;
					case SHORTCUT_CYCLE_EFFECT:
						screen_effect = (screen_effect + 1) % config.frontend.options[FE_OPT_EFFECT].count;
						Config_syncFrontend(config.frontend.options[FE_OPT_EFFECT].key, screen_effect);
						break;
					default: break;
				}

				if (mapping->mod) ignore_menu = 1;
			}
		}
	}

	if (!ignore_menu && PAD_justReleased(BTN_MENU)) {
		show_menu = 1;
	}

	// Two-pass button mapping: first collect buttons consumed by MENU+button
	// shortcuts this frame, then build the retro bitmask skipping consumed ones.
	int consumed[LOCAL_BUTTON_COUNT] = {0};
	for (int i = 0; i < SHORTCUT_COUNT; i++) {
		ButtonMapping *mapping = &config.shortcuts[i];
		if (!mapping->mod) continue;
		if (!PAD_isPressed(BTN_MENU)) continue;
		int btn = 1 << mapping->local;
		if (btn == BTN_NONE) continue;
		if (PAD_isPressed(btn) && mapping->local >= 0 && mapping->local < LOCAL_BUTTON_COUNT) {
			consumed[mapping->local] = 1;
		}
	}

	buttons = 0;
	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		int btn = 1 << mapping->local;
		if (btn==BTN_NONE) continue; // present buttons can still be unbound
		if (mapping->local >= 0 && mapping->local < LOCAL_BUTTON_COUNT && consumed[mapping->local]) continue;
		if (gamepad_type==0) {
			switch(btn) {
				case BTN_DPAD_UP:    btn = BTN_UP;    break;
				case BTN_DPAD_DOWN:  btn = BTN_DOWN;  break;
				case BTN_DPAD_LEFT:  btn = BTN_LEFT;  break;
				case BTN_DPAD_RIGHT: btn = BTN_RIGHT; break;
			}
		}
		if (PAD_isPressed(btn) && (!mapping->mod || PAD_isPressed(BTN_MENU))) {
			buttons |= 1 << mapping->retro;
			if (mapping->mod) ignore_menu = 1;
		}
		//  && !PWR_ignoreSettingInput(btn, show_setting)
	}

	// if (buttons) LOG_info("buttons: %i\n", buttons);
}
int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port==0 && device==RETRO_DEVICE_JOYPAD && index==0) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return buttons;
		return (buttons >> id) & 1;
	}
	else if (port==0 && device==RETRO_DEVICE_ANALOG) {
		if (index==RETRO_DEVICE_INDEX_ANALOG_LEFT) {
			if (id==RETRO_DEVICE_ID_ANALOG_X) return pad.laxis.x;
			else if (id==RETRO_DEVICE_ID_ANALOG_Y) return pad.laxis.y;
		}
		else if (index==RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
			if (id==RETRO_DEVICE_ID_ANALOG_X) return pad.raxis.x;
			else if (id==RETRO_DEVICE_ID_ANALOG_Y) return pad.raxis.y;
		}
	}
	return 0;
}
///////////////////////////////

void Input_init(const struct retro_input_descriptor *vars) {
	static int input_initialized = 0;
	if (input_initialized) return;

	LOG_info("Input_init\n");

	config.controls = core_button_mapping[0].name ? core_button_mapping : default_button_mapping;

	puts("---------------------------------");

	const char* core_button_names[RETRO_BUTTON_COUNT] = {0};
	int present[RETRO_BUTTON_COUNT];
	int core_mapped = 0;
	if (vars) {
		core_mapped = 1;
		// identify buttons available in this core
		for (int i=0; vars[i].description; i++) {
			const struct retro_input_descriptor* var = &vars[i];
			if (var->port!=0 || var->device!=RETRO_DEVICE_JOYPAD || var->index!=0) continue;

			// TODO: don't ignore unavailable buttons, just override them to BTN_ID_NONE!
			if (var->id>=RETRO_BUTTON_COUNT) {
				//printf("UNAVAILABLE: %s\n", var->description); fflush(stdout);
				continue;
			}
			else {
				//printf("PRESENT    : %s\n", var->description); fflush(stdout);
			}
			present[var->id] = 1;
			core_button_names[var->id] = var->description;
		}
	}

	puts("---------------------------------");

	for (int i=0;default_button_mapping[i].name; i++) {
		ButtonMapping* mapping = &default_button_mapping[i];
		//LOG_info("DEFAULT %s (%s): <%s>\n", core_button_names[mapping->retro], mapping->name, (mapping->local==BTN_ID_NONE ? "NONE" : device_button_names[mapping->local]));
		if (core_button_names[mapping->retro]) mapping->name = (char*)core_button_names[mapping->retro];
	}

	puts("---------------------------------");

	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		mapping->default_ = mapping->local;

		// ignore mappings that aren't available in this core
		if (core_mapped && !present[mapping->retro]) {
			mapping->ignore = 1;
			continue;
		}
		//LOG_info("%s: <%s> (%i:%i)\n", mapping->name, (mapping->local==BTN_ID_NONE ? "NONE" : device_button_names[mapping->local]), mapping->local, mapping->retro);
	}

	puts("---------------------------------");
	input_initialized = 1;
}

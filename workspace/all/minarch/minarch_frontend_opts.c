#include "minarch_internal.h"
#include "minarch_frontend_opts.h"
#include "minarch_cheats.h"
#include "ra_integration.h"
#include "notification.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libgen.h>

int Menu_messageWithFont(char* message, char** pairs, TTF_Font* f) {
	GFX_setMode(MODE_MAIN);
	int dirty = 1;
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) break;

		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);


		GFX_clear(screen);
		GFX_blitMessage(f, message, screen, &(SDL_Rect){SCALE1(PADDING),SCALE1(PADDING),screen->w-SCALE1(2*PADDING),screen->h-SCALE1(PILL_SIZE+PADDING)});
		GFX_blitButtonGroup(pairs, 0, screen, 1);
		GFX_flip(screen);
		dirty = 0;


		hdmimon();
	}
	GFX_setMode(MODE_MENU);
	return MENU_CALLBACK_NOP; // TODO: this should probably be an arg
}

int Menu_message(char* message, char** pairs) {
	return Menu_messageWithFont(message, pairs, font.medium);
}

static int MenuList_freeItems(MenuList* list, int i) {
	// TODO: what calls this? do menu's register for needing it? then call it on quit for each?
	if (list->items) free(list->items);
	return MENU_CALLBACK_NOP;
}

static int OptionFrontend_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Config_syncFrontend(item->key, item->value);
	return MENU_CALLBACK_NOP;
}
static MenuList OptionFrontend_menu = {
	.type = MENU_VAR,
	.on_change = OptionFrontend_optionChanged,
	.items = NULL,
};
int OptionFrontend_openMenu(MenuList* list, int i) {
	if (OptionFrontend_menu.items==NULL) {
		// TODO: where do I free this? I guess I don't :sweat_smile:
		if (!config.frontend.enabled_count) {
			int enabled_count = 0;
			for (int i=0; i<config.frontend.count; i++) {
				if (!config.frontend.options[i].lock) enabled_count += 1;
			}
			config.frontend.enabled_count = enabled_count;
			config.frontend.enabled_options = calloc(enabled_count+1, sizeof(Option*));
			int j = 0;
			for (int i=0; i<config.frontend.count; i++) {
				Option* item = &config.frontend.options[i];
				if (item->lock) continue;
				config.frontend.enabled_options[j] = item;
				j += 1;
			}
		}
		OptionFrontend_menu.items = calloc(config.frontend.enabled_count+1, sizeof(MenuItem));
		for (int j=0; j<config.frontend.enabled_count; j++) {
			Option* option = config.frontend.enabled_options[j];
			MenuItem* item = &OptionFrontend_menu.items[j];
			item->key = option->key;
			item->name = option->name;
			item->desc = option->desc;
			item->value = option->value;
			item->values = option->labels;
		}
	}
	else {
		// update values
		for (int j=0; j<config.frontend.enabled_count; j++) {
			Option* option = config.frontend.enabled_options[j];
			MenuItem* item = &OptionFrontend_menu.items[j];
			item->value = option->value;
		}
	}
	Menu_options(&OptionFrontend_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionEmulator_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Option* option = OptionList_getOption(&config.core, item->key);
	LOG_info("%s (%s) changed from `%s` (%s) to `%s` (%s)\n", item->name, item->key,
		item->values[option->value], option->values[option->value],
		item->values[item->value], option->values[item->value]
	);
	OptionList_setOptionRawValue(&config.core, item->key, item->value);
	return MENU_CALLBACK_NOP;
}

static int OptionEmulator_optionDetail(MenuList* list, int i);
int OptionEmulator_openMenu(MenuList* list, int index);

static MenuList OptionEmulator_menu = {
	.type = MENU_FIXED,
	.on_confirm = OptionEmulator_optionDetail, // TODO: this needs pagination to be truly useful
	.on_change = OptionEmulator_optionChanged,
	.items = NULL,
};

static int OptionEmulator_optionDetail(MenuList* list, int i) {
	MenuItem* item = &list->items[i];

	if (item->values == NULL) {
		// This is a category item
		// Display the corresponding submenu
		list->category = item->key;
		LOG_info("%s: displaying category %s\n", __FUNCTION__, item->key);

		int prev_enabled_count = config.core.enabled_count;
		Option **prev_enabled = config.core.enabled_options;
		MenuItem *prev_items = OptionEmulator_menu.items;

		OptionEmulator_openMenu(list, 0);
		list->category = NULL;

		config.core.enabled_count = prev_enabled_count ;
		config.core.enabled_options = prev_enabled ;
		OptionEmulator_menu.items = prev_items;

		LOG_info("%s: back to root menu\n", __FUNCTION__);
	}
	else {
		Option* option = OptionList_getOption(&config.core, item->key);
		if (option->full) return Menu_messageWithFont(option->full, (char*[]){ "B","BACK", NULL }, font.medium);
		else return MENU_CALLBACK_NOP;
	}
}

int OptionEmulator_openMenu(MenuList* list, int index) {
	LOG_info("%s: limit to category %s\n", __FUNCTION__, list->category ? list->category : "<all>");

	if (list->category == NULL) {
		if (core.update_visibility_callback) {
			LOG_info("%s: calling update visibility callback\n", __FUNCTION__);
			core.update_visibility_callback();
		}
	}

	int enabled_count = 0;
	config.core.enabled_options = calloc(config.core.count + 1, sizeof(Option*));
	for (int i=0; i<config.core.count; i++) {
		Option *item = &config.core.options[i];

		// Exclude locked and hidden items
		if (item->lock || item->hidden) {
			continue;
		}
		// Restrict to the current category
		if (list->category == NULL && item->category) {
			continue;
		}
		if (list->category && (item->category == NULL || strcmp(item->category, list->category))) {
			continue;
		}

		config.core.enabled_options[enabled_count++] = item;
	}
	config.core.enabled_count = enabled_count;
	config.core.enabled_options = realloc(config.core.enabled_options, sizeof(Option *) * (enabled_count + 1));

	// If we are at the top level, add the categories
	int cat_count = 0;

	if (list->category == NULL && config.core.categories) {
		while (config.core.categories[cat_count].key) {
			cat_count++;
		}
	}

	OptionEmulator_menu.items = calloc(cat_count + config.core.enabled_count + 1, sizeof(MenuItem));

	for (int i=0; i<cat_count; i++) {
		OptionCategory *cat = &config.core.categories[i];
		MenuItem *item = &OptionEmulator_menu.items[i];
		item->key = cat->key;
		item->name = cat->desc;
		item->desc = cat->info;
	}

	for (int i=0; i<config.core.enabled_count; i++) {
		Option *option = config.core.enabled_options[i];
		MenuItem *item = &OptionEmulator_menu.items[cat_count + i];
		item->key = option->key;
		item->name = option->name;
		item->desc = option->desc;
		item->value = option->value;
		item->values = option->labels;
	}

	if (cat_count || config.core.enabled_count) {
		Menu_options(&OptionEmulator_menu);
		free(OptionEmulator_menu.items);
		free(config.core.enabled_options);
		OptionEmulator_menu.items = NULL;
		config.core.enabled_count = 0;
		config.core.enabled_options = NULL;
	}
	else {
		if (list->category) {
			Menu_message("This category has no options.", (char*[]){ "B","BACK", NULL });
		}
		else {
			Menu_message("This core has no options.", (char*[]){ "B","BACK", NULL });
		}
	}

	return MENU_CALLBACK_NOP;
}

static int OptionControls_bind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=button_labels) {
		// LOG_info("changed gamepad_type\n");
		return MENU_CALLBACK_NOP;
	}

	ButtonMapping* button = &config.controls[item->id];

	int bound = 0;
	while (!bound) {
		GFX_startFrame();
		PAD_poll();

		// NOTE: off by one because of the initial NONE value
		for (int id=0; id<=LOCAL_BUTTON_COUNT; id++) {
			if (PAD_justPressed(1 << (id-1))) {
				item->value = id;
				button->local = id - 1;
				if (PAD_isPressed(BTN_MENU)) {
					item->value += LOCAL_BUTTON_COUNT;
					button->mod = 1;
				}
				else {
					button->mod = 0;
				}
				bound = 1;
				break;
			}
		}
		GFX_delay();
		hdmimon();
	}
	return MENU_CALLBACK_NEXT_ITEM;
}
static int OptionControls_unbind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=button_labels) return MENU_CALLBACK_NOP;

	ButtonMapping* button = &config.controls[item->id];
	button->local = -1;
	button->mod = 0;
	return MENU_CALLBACK_NOP;
}
static int OptionControls_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=gamepad_labels) return MENU_CALLBACK_NOP;

	if (has_custom_controllers) {
		gamepad_type = item->value;
		int device = strtol(gamepad_values[item->value], NULL, 0);
		core.set_controller_port_device(0, device);
	}
	return MENU_CALLBACK_NOP;
}
static MenuList OptionControls_menu = {
	.type = MENU_INPUT,
	.desc = "Press A to set and X to clear."
		"\nSupports single button and MENU+button." // TODO: not supported on nano because POWER doubles as MENU
	,
	.on_confirm = OptionControls_bind,
	.on_change = OptionControls_unbind,
	.items = NULL
};

int OptionControls_openMenu(MenuList* list, int i) {
	LOG_info("OptionControls_openMenu\n");

	if (OptionControls_menu.items==NULL) {

		// TODO: where do I free this?
		OptionControls_menu.items = calloc(RETRO_BUTTON_COUNT+1+has_custom_controllers, sizeof(MenuItem));
		int k = 0;

		if (has_custom_controllers) {
			MenuItem* item = &OptionControls_menu.items[k++];
			item->name = "Controller";
			item->desc = "Select the type of controller.";
			item->value = gamepad_type;
			item->values = gamepad_labels;
			item->on_change = OptionControls_optionChanged;
		}

		for (int j=0; config.controls[j].name; j++) {
			ButtonMapping* button = &config.controls[j];
			if (button->ignore) continue;

			//LOG_info("\t%s (%i:%i)\n", button->name, button->local, button->retro);

			MenuItem* item = &OptionControls_menu.items[k++];
			item->id = j;
			item->name = button->name;
			item->desc = NULL;
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
			item->values = button_labels;
		}
	}
	else {
		// update values
		int k = 0;

		if (has_custom_controllers) {
			MenuItem* item = &OptionControls_menu.items[k++];
			item->value = gamepad_type;
		}

		for (int j=0; config.controls[j].name; j++) {
			ButtonMapping* button = &config.controls[j];
			if (button->ignore) continue;

			MenuItem* item = &OptionControls_menu.items[k++];
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
		}
	}
	Menu_options(&OptionControls_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionShortcuts_bind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	ButtonMapping* button = &config.shortcuts[item->id];
	int bound = 0;
	while (!bound) {
		GFX_startFrame();
		PAD_poll();

		// NOTE: off by one because of the initial NONE value
		for (int id=0; id<=LOCAL_BUTTON_COUNT; id++) {
			if (PAD_justPressed(1 << (id-1))) {
				item->value = id;
				button->local = id - 1;
				if (PAD_isPressed(BTN_MENU)) {
					item->value += LOCAL_BUTTON_COUNT;
					button->mod = 1;
				}
				else {
					button->mod = 0;
				}
				bound = 1;
				break;
			}
		}
		GFX_delay();
		hdmimon();
	}
	return MENU_CALLBACK_NEXT_ITEM;
}
static int OptionShortcuts_unbind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	ButtonMapping* button = &config.shortcuts[item->id];
	button->local = -1;
	button->mod = 0;
	return MENU_CALLBACK_NOP;
}
static MenuList OptionShortcuts_menu = {
	.type = MENU_INPUT,
	.desc = "Press A to set and X to clear."
		"\nSupports single button and MENU+button." // TODO: not supported on nano because POWER doubles as MENU
	,
	.on_confirm = OptionShortcuts_bind,
	.on_change = OptionShortcuts_unbind,
	.items = NULL
};
char* getSaveDesc(void) {
	switch (config.loaded) {
		case CONFIG_NONE:		return "Using defaults."; break;
		case CONFIG_CONSOLE:	return "Using console config."; break;
		case CONFIG_GAME:		return "Using game config."; break;
	}
	return NULL;
}
int OptionShortcuts_openMenu(MenuList* list, int i) {
	if (OptionShortcuts_menu.items==NULL) {
		// TODO: where do I free this? I guess I don't :sweat_smile:
		OptionShortcuts_menu.items = calloc(SHORTCUT_COUNT+1, sizeof(MenuItem));
		for (int j=0; config.shortcuts[j].name; j++) {
			ButtonMapping* button = &config.shortcuts[j];
			MenuItem* item = &OptionShortcuts_menu.items[j];
			item->id = j;
			item->name = button->name;
			item->desc = NULL;
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
			item->values = button_labels;
		}
	}
	else {
		// update values
		for (int j=0; config.shortcuts[j].name; j++) {
			ButtonMapping* button = &config.shortcuts[j];
			MenuItem* item = &OptionShortcuts_menu.items[j];
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
		}
	}
	Menu_options(&OptionShortcuts_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionSaveChanges_onConfirm(MenuList* list, int i) {
	char* message;
	switch (i) {
		case 0: {
			Config_write(CONFIG_WRITE_ALL);
			message = "Saved for console.";
			break;
		}
		case 1: {
			Config_write(CONFIG_WRITE_GAME);
			message = "Saved for game.";
			break;
		}
		default: {
			Config_restore();
			if (config.loaded) message = "Restored console defaults.";
			else message = "Restored defaults.";
			break;
		}
	}
	Menu_message(message, (char*[]){ "A","OKAY", NULL });
	OptionSaveChanges_updateDesc();
	return MENU_CALLBACK_EXIT;
}
static MenuList OptionSaveChanges_menu = {
	.type = MENU_LIST,
	.on_confirm = OptionSaveChanges_onConfirm,
	.items = (MenuItem[]){
		{"Save for console"},
		{"Save for game"},
		{"Restore defaults"},
		{NULL},
	}
};
int OptionSaveChanges_openMenu(MenuList* list, int i) {
	OptionSaveChanges_updateDesc();
	OptionSaveChanges_menu.desc = getSaveDesc();
	Menu_options(&OptionSaveChanges_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionQuicksave_onConfirm(MenuList* list, int i) {
	Menu_beforeSleep();
	PWR_powerOff(0);
}

static int OptionCheats_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	struct Cheat *cheat = &cheatcodes.cheats[i];

	// Block enabling cheats in RetroAchievements hardcore mode
	if (RA_isHardcoreModeActive() && item->value) {
		LOG_info("Cheat enable blocked - hardcore mode active\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, "Cheats disabled in Hardcore mode", NULL);
		item->value = 0; // Revert the toggle
		return MENU_CALLBACK_NOP;
	}

	cheat->enabled = item->value;
	Core_applyCheats(&cheatcodes);
	return MENU_CALLBACK_NOP;
}

static int OptionCheats_optionDetail(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	struct Cheat *cheat = &cheatcodes.cheats[i];
	if (cheat->info)
		return Menu_message((char*)cheat->info, (char*[]){ "B","BACK", NULL });
	else return MENU_CALLBACK_NOP;
}

static MenuList OptionCheats_menu = {
	.type = MENU_FIXED,
	.on_confirm = OptionCheats_optionDetail, // TODO: this needs pagination to be truly useful
	.on_change = OptionCheats_optionChanged,
	.items = NULL,
};
int OptionCheats_openMenu(MenuList* list, int i) {
	if (OptionCheats_menu.items == NULL) {
		// populate
		OptionCheats_menu.items = calloc(cheatcodes.count + 1, sizeof(MenuItem));
		for (int i = 0; i<cheatcodes.count; i++) {
			struct Cheat *cheat = &cheatcodes.cheats[i];
			MenuItem *item = &OptionCheats_menu.items[i];

			// this stuff gets actually copied around.. what year is it?
			int len = strlen(cheat->name) + 1;
			item->name = calloc(len, sizeof(char));
			strcpy(item->name, cheat->name);

			if(cheat->info) {
				len = strlen(cheat->info) + 1;
				item->desc = calloc(len, sizeof(char));
				strncpy(item->desc, cheat->info, len);
				GFX_wrapText(font.tiny, item->desc, DEVICE_WIDTH - SCALE1(2*PADDING), 2);
			}

			item->value = cheat->enabled;
			item->values = onoff_labels;
		}
	}
	else {
		// update
		for (int j = 0; j < cheatcodes.count; j++) {
			struct Cheat *cheat = &cheatcodes.cheats[j];
			MenuItem *item = &OptionCheats_menu.items[j];
			// I guess that makes sense, nobody is changing these but us - what about state restore?
			if(!cheat->enabled)
				continue;
			item->value = cheat->enabled;
		}
	}

	if (OptionCheats_menu.items[0].name) {
		Menu_options(&OptionCheats_menu);
	}
	else {
		// we expect at most CHEAT_MAX_PATHS paths with MAX_PATH length, just hardcode it here
		char paths[CHEAT_MAX_PATHS][MAX_PATH];
		int count = 0;
		Cheat_getPaths(paths, &count);

		// concatenate all paths into one string, and prepend title "No cheat file loaded.\n\n"
		// each path on its own line, and remove the absolute path prefix
		char cheats_path[CHEAT_MAX_LIST_LENGTH] = {0};

		// prepend title with bounds checking
		const char* title = "No cheat file loaded.\n\n";
		size_t title_len = strlen(title);

		strcpy(cheats_path, title);  // Use strcpy for first string
		size_t current_len = title_len;

		for (int i = 0; i < count && i < CHEAT_MAX_DISPLAY_PATHS; i++) {
			char* p = basename(paths[i]);

			// Check for NULL return from basename
			if (p == NULL) {
				LOG_info("basename() returned NULL for path: %s\n", paths[i]);
				continue;
			}

			size_t p_len = strlen(p);
			size_t newline_len = (i < count - 1) ? 1 : 0;  // "\n" length

			// Check if adding this path would overflow the buffer
			if (current_len + p_len + newline_len >= CHEAT_MAX_LIST_LENGTH) {
				LOG_info("Cheats path buffer would overflow, truncating list\n");
				strcat(cheats_path, "...");
				break;
			}

			// Safe to append
			strcat(cheats_path, p);
			current_len += p_len;

			if (i < count - 1) {
				strcat(cheats_path, "\n");
				current_len += 1;
			}
		}

		Menu_messageWithFont(cheats_path, (char*[]){ "B","BACK", NULL }, font.small);
	}

	return MENU_CALLBACK_NOP;
}

static int OptionPragmas_optionChanged(MenuList* list, int i) {
		MenuItem* item = &list->items[i];
		for (int shader_index=0; shader_index < config.shaders.options[SH_NROFSHADERS].value; shader_index++) {
			ShaderParam *params = PLAT_getShaderPragmas(shader_index);
			for (int j = 0; j < 32; j++) {
				if (exactMatch(params[j].name, item->key)) {
					params[j].value = strtof(item->values[item->value], NULL);
				}
			}
		}
		int global_index = 0;
		for (int y = 0; y < SH_NROFSHADERS; y++) {
			for (int j = 0; j < config.shaderpragmas[y].count; j++) {
				MenuItem* item = &list->items[global_index];
				config.shaderpragmas[y].options[j].value = item->value;
				global_index++;
			}
		}
		return MENU_CALLBACK_NOP;
}

static MenuList PragmasOptions_menu = {
	.type = MENU_FIXED,
	.on_confirm = NULL,
	.on_change = OptionPragmas_optionChanged,
	.items = NULL
};
static int OptionPragmas_openMenu(MenuList* list, int i) {
	int progressCount = 0;
	int totalcount = 0;
	for (int y=0; y < config.shaders.options[SH_NROFSHADERS].value; y++) {
		totalcount += config.shaderpragmas[y].count;
	}
	PragmasOptions_menu.items = calloc(totalcount + 1, sizeof(MenuItem));
	for (int y=0; y < config.shaders.options[SH_NROFSHADERS].value; y++) {
		for (int j = 0; j < config.shaderpragmas[y].count; j++) {
			MenuItem* item = &PragmasOptions_menu.items[progressCount];
			Option* configitem = &config.shaderpragmas[y].options[j];
			item->id = j;
			item->name = configitem->name;
			item->desc = configitem->desc;
			item->value = configitem->value;
			item->key = configitem->key;
			item->values = configitem->values;
			progressCount++;
		}
	}

	if (PragmasOptions_menu.items[0].name) {
		Menu_options(&PragmasOptions_menu);
	} else {
		Menu_message("No extra settings found", (char*[]){"B", "BACK", NULL});
	}

	return MENU_CALLBACK_NOP;
}
static int OptionShaders_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	// Process menu entry change, update underlying config cruft and call handler
	Config_syncShaders(item->key, item->value);
	// Apply shader pragmas if needed
	applyShaderSettings();
	// Update menu entries to reflect any changes made by the handler
	for (int y = 0; y < config.shaders.count; y++) {
		MenuItem* item = &list->items[y];
		item->value = config.shaders.options[y].value;
	}

	if(i==SH_SHADERS_PRESET) {
		// On shader preset change:
		// Push all new shader settings to shader engine,
		// compile shaders if needed, populate pragmas list
		initShaders();

		// Now that we have a list of shader parameters,
		// re-read shader preset file to set pragma values in-menu
		Config_syncShaders(item->key, item->value);

		// Push parameters to shader engine
		applyShaderSettings();
	}
	return MENU_CALLBACK_NOP;
}

static MenuList ShaderOptions_menu = {
	.type = MENU_FIXED,
	.on_confirm = NULL,
	.on_change = OptionShaders_optionChanged,
	.items = NULL
};

int OptionShaders_openMenu(MenuList* list, int i) {
	int filecount;
	char** filelist = list_files_in_folder(SHADERS_FOLDER "/glsl", &filecount,NULL,NULL);

	// Check if folder read failed or no files found
	if (!filelist || filecount == 0) {
		Menu_message("No shaders available\n/Shaders folder or shader files not found", (char*[]){"B", "BACK", NULL});
		return MENU_CALLBACK_NOP;
	}

	ShaderOptions_menu.items = calloc(config.shaders.count + 1, sizeof(MenuItem));
	for (int i = 0; i < config.shaders.count; i++) {
		MenuItem* item = &ShaderOptions_menu.items[i];
		Option* configitem = &config.shaders.options[i];
		item->id = i;
		item->name = configitem->name;
		item->desc = configitem->desc;
		item->value = configitem->value;
		item->key = configitem->key;
		if(i == SH_EXTRASETTINGS) item->on_confirm = OptionPragmas_openMenu;

		if (strcmp(config.shaders.options[i].key, "minarch_shader1") == 0 ||
			strcmp(config.shaders.options[i].key, "minarch_shader2") == 0 ||
			strcmp(config.shaders.options[i].key, "minarch_shader3") == 0) {
			item->values = filelist;
			config.shaders.options[i].values = filelist;
		} else {
			item->values = config.shaders.options[i].values;
		}
	}


	if (ShaderOptions_menu.items[0].name) {
		Menu_options(&ShaderOptions_menu);
	} else {
		Menu_message("No shaders available\n/Shaders folder or shader files not found", (char*[]){"B", "BACK", NULL});
	}

	return MENU_CALLBACK_NOP;
}

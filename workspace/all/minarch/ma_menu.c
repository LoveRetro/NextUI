#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <rcheevos/rc_client.h>
#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "notification.h"
#include "ra_integration.h"
#include "ra_badges.h"
#include "i18n.h"
#include "ma_internal.h"
#include "ma_cheats.h"
#include "ma_saves.h"
#include "ma_game.h"
#include "ma_config.h"
#include "ma_video.h"
#include "ma_frontend_opts.h"
#include "ma_menu.h"

///////////////////////////////

// TODO: this is a dumb API
SDL_Surface* digits;
#define DIGIT_WIDTH 9
#define DIGIT_HEIGHT 8
#define DIGIT_TRACKING -2
enum {
	DIGIT_SLASH = 10,
	DIGIT_DOT,
	DIGIT_PERCENT,
	DIGIT_X,
	DIGIT_OP, // (
	DIGIT_CP, // )
	DIGIT_COUNT,
};
#define DIGIT_SPACE DIGIT_COUNT
void MSG_init(void) {
	digits = SDL_CreateRGBSurface(SDL_SWSURFACE,SCALE2(DIGIT_WIDTH*DIGIT_COUNT,DIGIT_HEIGHT),FIXED_DEPTH, 0,0,0,0);
	SDL_FillRect(digits, NULL, RGB_BLACK);
	
	SDL_Surface* digit;
	char* chars[] = { "0","1","2","3","4","5","6","7","8","9","/",".","%","x","(",")", NULL };
	char* c;
	int i = 0;
	while ((c = chars[i])) {
		digit = TTF_RenderUTF8_Blended(font.tiny, c, COLOR_WHITE);
		SDL_BlitSurface(digit, NULL, digits, &(SDL_Rect){ (i * SCALE1(DIGIT_WIDTH)) + (SCALE1(DIGIT_WIDTH) - digit->w)/2, (SCALE1(DIGIT_HEIGHT) - digit->h)/2});
		SDL_FreeSurface(digit);
		i += 1;
	}
}
static int MSG_blitChar(int n, int x, int y) {
	if (n!=DIGIT_SPACE) SDL_BlitSurface(digits, &(SDL_Rect){n*SCALE1(DIGIT_WIDTH),0,SCALE2(DIGIT_WIDTH,DIGIT_HEIGHT)}, screen, &(SDL_Rect){x,y});
	return x + SCALE1(DIGIT_WIDTH + DIGIT_TRACKING);
}
static int MSG_blitInt(int num, int x, int y) {
	int i = num;
	int n;
	
	if (i > 999) {
		n = i / 1000;
		i -= n * 1000;
		x = MSG_blitChar(n,x,y);
	}
	if (i > 99) {
		n = i / 100;
		i -= n * 100;
		x = MSG_blitChar(n,x,y);
	}
	else if (num>99) {
		x = MSG_blitChar(0,x,y);
	}
	if (i > 9) {
		n = i / 10;
		i -= n * 10;
		x = MSG_blitChar(n,x,y);
	}
	else if (num>9) {
		x = MSG_blitChar(0,x,y);
	}
	
	n = i;
	x = MSG_blitChar(n,x,y);
	
	return x;
}
static int MSG_blitDouble(double num, int x, int y) {
	int i = num;
	int r = (num-i) * 10;
	int n;
	
	x = MSG_blitInt(i, x,y);

	n = DIGIT_DOT;
	x = MSG_blitChar(n,x,y);
	
	n = r;
	x = MSG_blitChar(n,x,y);
	return x;
}
void MSG_quit(void) {
	SDL_FreeSurface(digits);
}

///////////////////////////////


///////////////////////////////////////

#define MENU_ITEM_COUNT 5
#define MENU_SLOT_COUNT 8

enum {
	ITEM_CONT,
	ITEM_SAVE,
	ITEM_LOAD,
	ITEM_OPTS,
	ITEM_QUIT,
};

enum {
	STATUS_CONT =  0,
	STATUS_SAVE =  1,
	STATUS_LOAD = 11,
	STATUS_OPTS = 23,
	STATUS_DISC = 24,
	STATUS_QUIT = 30,
	STATUS_RESET= 31,
};

// TODO: I don't love how overloaded this has become
static struct {
	SDL_Surface* bitmap;
	SDL_Surface* overlay;
	char* items[MENU_ITEM_COUNT];
	char* disc_paths[9]; // up to 9 paths, Arc the Lad Collection is 7 discs
	char minui_dir[256];
	char slot_path[256];
	char base_path[256];
	char bmp_path[256];
	char txt_path[256];
	int disc;
	int total_discs;
	int slot;
	int save_exists;
	int preview_exists;
} menu = {
	.bitmap = NULL,
	.disc = -1,
	.total_discs = 0,
	.save_exists = 0,
	.preview_exists = 0,
	
	.items = {
		[ITEM_CONT] = "Continue",
		[ITEM_SAVE] = "Save",
		[ITEM_LOAD] = "Load",
		[ITEM_OPTS] = "Options",
		[ITEM_QUIT] = "Quit",
	}
};

void Menu_init(void) {
	menu.overlay = SDL_CreateRGBSurfaceWithFormat(SDL_SWSURFACE,
		DEVICE_WIDTH,DEVICE_HEIGHT,
		screen->format->BitsPerPixel,screen->format->format);
	SDL_SetSurfaceBlendMode(menu.overlay, SDL_BLENDMODE_BLEND);
	Uint32 color = SDL_MapRGBA(menu.overlay->format, 0, 0, 0, 0);
	SDL_FillRect(screen, NULL, color);
	
	char emu_name[256];
	getEmuName(game.path, emu_name);
	sprintf(menu.minui_dir, SHARED_USERDATA_PATH "/.minui/%s", emu_name);
	mkdir(menu.minui_dir, 0755);

	// always sanitized/outer name, to keep main UI from having to inspect archives
	sprintf(menu.slot_path, "%s/%s.txt", menu.minui_dir, game.name);
	
	menu.items[ITEM_CONT] = T("menu.continue");
	menu.items[ITEM_SAVE] = T("menu.save");
	menu.items[ITEM_LOAD] = T("menu.load");
	menu.items[ITEM_OPTS] = T("menu.options");
	menu.items[ITEM_QUIT] = T("menu.quit");

	if (simple_mode) menu.items[ITEM_OPTS] = T("menu.reset");
	
	if (game.m3u_path[0]) {
		char* tmp;
		strcpy(menu.base_path, game.m3u_path);
		tmp = strrchr(menu.base_path, '/') + 1;
		tmp[0] = '\0';
		
		//read m3u file
		FILE* file = fopen(game.m3u_path, "r");
		if (file) {
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines
		
				char disc_path[256];
				strcpy(disc_path, menu.base_path);
				tmp = disc_path + strlen(disc_path);
				strcpy(tmp, line);
				
				// found a valid disc path
				if (exists(disc_path)) {
					menu.disc_paths[menu.total_discs] = strdup(disc_path);
					// matched our current disc
					if (exactMatch(disc_path, game.path)) {
						menu.disc = menu.total_discs;
					}
					menu.total_discs += 1;
				}
			}
			fclose(file);
		}
	}
}
void Menu_quit(void) {
	SDL_FreeSurface(menu.overlay);
}
void Menu_beforeSleep() {
	SRAM_write();
	RTC_write();
	State_autosave();
	putFile(AUTO_RESUME_PATH, game.path + strlen(SDCARD_PATH));
}
void Menu_afterSleep() {
	unlink(AUTO_RESUME_PATH);
	setOverclock(overclock);
}


// Achievement menu state (stored so on_confirm handler can access achievement data)
static const rc_client_achievement_list_t* ach_menu_list = NULL;
static const rc_client_achievement_t** ach_menu_achievements = NULL; // Flattened array for index lookup
static int ach_menu_count = 0;
static bool ach_filter_locked_only = false;  // Y button toggle: show all or locked only

// Achievement sorting comparison function
static int ach_compare_unlocked_first(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	// Unlocked achievements come first
	if (achA->unlocked != achB->unlocked) return achB->unlocked - achA->unlocked;
	return 0;
}

static int ach_compare_display_first(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	// Lower display order first
	return (int)achA->id - (int)achB->id;  // ID is a reasonable proxy for display order
}

static int ach_compare_display_last(const void* a, const void* b) {
	return -ach_compare_display_first(a, b);
}

static int ach_compare_won_by_most(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	// Higher rarity (more common) first
	if (achA->rarity != achB->rarity) return (achB->rarity - achA->rarity) > 0 ? 1 : -1;
	return 0;
}

static int ach_compare_won_by_least(const void* a, const void* b) {
	return -ach_compare_won_by_most(a, b);
}

static int ach_compare_points_most(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return (int)achB->points - (int)achA->points;
}

static int ach_compare_points_least(const void* a, const void* b) {
	return -ach_compare_points_most(a, b);
}

static int ach_compare_title_az(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return strcmp(achA->title, achB->title);
}

static int ach_compare_title_za(const void* a, const void* b) {
	return -ach_compare_title_az(a, b);
}

static int ach_compare_type_asc(const void* a, const void* b) {
	const rc_client_achievement_t* achA = *(const rc_client_achievement_t**)a;
	const rc_client_achievement_t* achB = *(const rc_client_achievement_t**)b;
	return (int)achA->type - (int)achB->type;
}

static int ach_compare_type_desc(const void* a, const void* b) {
	return -ach_compare_type_asc(a, b);
}

static void ach_sort_achievements(const rc_client_achievement_t** achievements, int count) {
	int sort_order = CFG_getRAAchievementSortOrder();
	int (*compare)(const void*, const void*) = NULL;
	
	switch (sort_order) {
		case RA_SORT_UNLOCKED_FIRST: compare = ach_compare_unlocked_first; break;
		case RA_SORT_DISPLAY_ORDER_FIRST: compare = ach_compare_display_first; break;
		case RA_SORT_DISPLAY_ORDER_LAST: compare = ach_compare_display_last; break;
		case RA_SORT_WON_BY_MOST: compare = ach_compare_won_by_most; break;
		case RA_SORT_WON_BY_LEAST: compare = ach_compare_won_by_least; break;
		case RA_SORT_POINTS_MOST: compare = ach_compare_points_most; break;
		case RA_SORT_POINTS_LEAST: compare = ach_compare_points_least; break;
		case RA_SORT_TITLE_AZ: compare = ach_compare_title_az; break;
		case RA_SORT_TITLE_ZA: compare = ach_compare_title_za; break;
		case RA_SORT_TYPE_ASC: compare = ach_compare_type_asc; break;
		case RA_SORT_TYPE_DESC: compare = ach_compare_type_desc; break;
		default: compare = ach_compare_unlocked_first; break;
	}
	
	if (compare && count > 1) {
		qsort((void*)achievements, count, sizeof(rc_client_achievement_t*), compare);
	}
}

static int OptionAchievements_showDetail(MenuList* list, int i) {
	if (!ach_menu_achievements || i < 0 || i >= ach_menu_count) {
		return MENU_CALLBACK_NOP;
	}
	
	const rc_client_achievement_t* ach = ach_menu_achievements[i];
	if (!ach || !ach->title) {
		return MENU_CALLBACK_NOP;
	}
	
	GFX_setMode(MODE_MAIN);
	int dirty = 1;
	int show_detail = 1;
	
	while (show_detail) {
		GFX_startFrame();
		PAD_poll();
		
		// Check for input
		if (PAD_justPressed(BTN_B)) {
			show_detail = 0;
		} else if (PAD_justPressed(BTN_X)) {
			// Toggle mute for this achievement
			RA_toggleAchievementMute(ach->id);
			dirty = 1;
		} else if (PAD_justPressed(BTN_LEFT) || PAD_justRepeated(BTN_LEFT)) {
			// Navigate to previous achievement (with wrap-around)
			i = (i - 1 + ach_menu_count) % ach_menu_count;
			ach = ach_menu_achievements[i];
			dirty = 1;
		} else if (PAD_justPressed(BTN_RIGHT) || PAD_justRepeated(BTN_RIGHT)) {
			// Navigate to next achievement (with wrap-around)
			i = (i + 1) % ach_menu_count;
			ach = ach_menu_achievements[i];
			dirty = 1;
		}
		
		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);
		
		if (dirty) {
			bool is_muted = RA_isAchievementMuted(ach->id);
			bool is_offline_pending = RA_isAchievementOfflinePending(ach->id);
			
			GFX_clear(screen);
			
			// Layout: badge icon centered at top, then title, then details
			int badge_size = SCALE1(64);  // 64px badge
			int content_y = SCALE1(PADDING) + SCALE1(6);  // Extra padding above icon
			int center_x = screen->w / 2;
			
			// Badge icon centered at top
			SDL_Surface* badge = RA_Badges_get(ach->badge_name, !ach->unlocked);
			if (badge) {
				SDL_Rect badge_src = {0, 0, badge->w, badge->h};
				SDL_Rect badge_dst = {
					center_x - badge_size / 2,
					content_y,
					badge_size, badge_size
				};
				SDL_BlitScaled(badge, &badge_src, screen, &badge_dst);
				content_y += badge_size + SCALE1(6);
			}
			
			// Title centered - wrap to max 2 lines with ellipsis if needed
			int max_text_width = screen->w - SCALE1(PADDING * 2);
			content_y = GFX_blitWrappedText(font.medium, ach->title, max_text_width, 2, COLOR_WHITE, screen, content_y);
			content_y += SCALE1(2);  // Spacing after title
			
			// Description - unlimited lines
			content_y = GFX_blitWrappedText(font.small, ach->description, max_text_width, 0, COLOR_WHITE, screen, content_y);
			content_y += SCALE1(4);  // Spacing after description
			
			// Points (singular/plural) - use tiny font like other metadata
			char points_str[32];
			if (ach->points == 1) {
				snprintf(points_str, sizeof(points_str), "1 point");
			} else {
				snprintf(points_str, sizeof(points_str), "%u points", ach->points);
			}
			SDL_Surface* points_text = TTF_RenderUTF8_Blended(font.tiny, points_str, COLOR_LIGHT_TEXT);
			SDL_BlitSurface(points_text, NULL, screen, &(SDL_Rect){
				center_x - points_text->w / 2, content_y
			});
			content_y += points_text->h + SCALE1(2);
			SDL_FreeSurface(points_text);
			
			// Unlock time or progress (smaller font, gray)
			if (ach->unlocked && ach->unlock_time > 0) {
				struct tm* tm_info = localtime(&ach->unlock_time);
				char time_buf[64];
				strftime(time_buf, sizeof(time_buf), "Unlocked %B %d %Y, %I:%M%p", tm_info);
				SDL_Surface* time_text = TTF_RenderUTF8_Blended(font.tiny, time_buf, COLOR_LIGHT_TEXT);
				SDL_BlitSurface(time_text, NULL, screen, &(SDL_Rect){
					center_x - time_text->w / 2, content_y
				});
				content_y += time_text->h + SCALE1(2);
				SDL_FreeSurface(time_text);
			} else if (ach->measured_progress[0]) {
				char progress_buf[64];
				snprintf(progress_buf, sizeof(progress_buf), "Progress: %s", ach->measured_progress);
				SDL_Surface* progress_text = TTF_RenderUTF8_Blended(font.tiny, progress_buf, COLOR_LIGHT_TEXT);
				SDL_BlitSurface(progress_text, NULL, screen, &(SDL_Rect){
					center_x - progress_text->w / 2, content_y
				});
				content_y += progress_text->h + SCALE1(2);
				SDL_FreeSurface(progress_text);
			}
			
			// Offline pending indicator
			if (is_offline_pending) {
				SDL_Surface* offline_text = TTF_RenderUTF8_Blended(font.tiny, T("ach.offline_pending"), COLOR_LIGHT_TEXT);
				int wifi_size = SCALE1(12);
				int total_w = wifi_size + SCALE1(4) + offline_text->w;
				int icon_x = center_x - total_w / 2;
				int text_x = icon_x + wifi_size + SCALE1(4);
				int wifi_y = content_y + (offline_text->h - wifi_size) / 2;
				GFX_blitAssetColor(ASSET_WIFI_OFF, NULL, screen,
				                   &(SDL_Rect){icon_x, wifi_y}, 0xCCCCCC);
				SDL_BlitSurface(offline_text, NULL, screen, &(SDL_Rect){
					text_x, content_y
				});
				content_y += offline_text->h + SCALE1(2);
				SDL_FreeSurface(offline_text);
			}
			
			// Unlock rate/rarity (smaller font, gray)
			if (ach->rarity > 0) {
				char rarity_buf[32];
				snprintf(rarity_buf, sizeof(rarity_buf), "%.2f%% unlock rate", ach->rarity);
				SDL_Surface* rarity_text = TTF_RenderUTF8_Blended(font.tiny, rarity_buf, COLOR_LIGHT_TEXT);
				SDL_BlitSurface(rarity_text, NULL, screen, &(SDL_Rect){
					center_x - rarity_text->w / 2, content_y
				});
				content_y += rarity_text->h + SCALE1(2);
				SDL_FreeSurface(rarity_text);
			}
			
			// Type tag
			const char* type_str = NULL;
			switch (ach->type) {
				case RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE:
					type_str = "[Missable]";
					break;
				case RC_CLIENT_ACHIEVEMENT_TYPE_PROGRESSION:
					type_str = "[Progression]";
					break;
				case RC_CLIENT_ACHIEVEMENT_TYPE_WIN:
					type_str = "[Win Condition]";
					break;
				default:
					break;
			}
			if (type_str) {
				SDL_Surface* type_text = TTF_RenderUTF8_Blended(font.tiny, type_str, COLOR_LIGHT_TEXT);
				SDL_BlitSurface(type_text, NULL, screen, &(SDL_Rect){
					center_x - type_text->w / 2, content_y
				});
				content_y += type_text->h + SCALE1(2);
				SDL_FreeSurface(type_text);
			}
			
			// Muted status below other info with gap before title
			if (is_muted) {
				SDL_Surface* mute_text = TTF_RenderUTF8_Blended(font.tiny, T("ach.muted_silenced"), COLOR_LIGHT_TEXT);
				int mute_icon_w = SCALE1(10);
				int mute_icon_h = SCALE1(12);
				int total_w = mute_icon_w + SCALE1(4) + mute_text->w;
				int icon_x = center_x - total_w / 2;
				int text_x = icon_x + mute_icon_w + SCALE1(4);
				int icon_y = content_y + SCALE1(4) + (mute_text->h - mute_icon_h) / 2;
				SDL_Rect mute_src = {0, SCALE1(4), SCALE1(10), SCALE1(12)};
				GFX_blitAssetColor(ASSET_VOLUME_MUTE, &mute_src, screen,
				                   &(SDL_Rect){icon_x, icon_y}, 0xCCCCCC);
				SDL_BlitSurface(mute_text, NULL, screen, &(SDL_Rect){
					text_x, content_y + SCALE1(4)
				});
				SDL_FreeSurface(mute_text);
			}
			
			// Button hints - update based on current mute state
			char* hints[] = {"X", is_muted ? T("btn.unmute") : T("btn.mute"), "B", T("btn.back"), NULL};
			GFX_blitButtonGroup(hints, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		
		hdmimon();
	}
	
	GFX_setMode(MODE_MENU);
	// Return the current index so caller can update selection
	return i;
}

static int OptionAchievements_openMenu(MenuList* list, int i) {
	if (!RA_isGameLoaded()) {
		Menu_message(T("ach.no_for_game"), (char*[]){"B",T("btn.back"), NULL});
		return MENU_CALLBACK_NOP;
	}

	uint32_t unlocked, total;
	RA_getAchievementSummary(&unlocked, &total);

	if (total == 0) {
		Menu_message(T("ach.no_available"), (char*[]){"B",T("btn.back"), NULL});
		return MENU_CALLBACK_NOP;
	}

	// Clean up any previous achievement list
	if (ach_menu_list) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
	}
	if (ach_menu_achievements) {
		free(ach_menu_achievements);
		ach_menu_achievements = NULL;
	}
	ach_menu_count = 0;

	// Create achievement list grouped by lock state
	ach_menu_list = (const rc_client_achievement_list_t*)RA_createAchievementList(
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);

	if (!ach_menu_list) {
		Menu_message(T("ach.failed_load"), (char*[]){"B",T("btn.back"), NULL});
		return MENU_CALLBACK_NOP;
	}

	// Count total achievements across all buckets and build flattened array
	int total_achievements = 0;
	for (uint32_t b = 0; b < ach_menu_list->num_buckets; b++) {
		total_achievements += ach_menu_list->buckets[b].num_achievements;
	}

	if (total_achievements == 0) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
		// This can happen with unsupported game versions where pseudo-achievements
		// are counted in the summary but not available in the achievement list
		Menu_message(T("ach.not_available"), (char*[]){"B",T("btn.back"), NULL});
		return MENU_CALLBACK_NOP;
	}

	// Create flattened array of all achievement pointers
	const rc_client_achievement_t** all_achievements = calloc(total_achievements, sizeof(rc_client_achievement_t*));
	int idx = 0;
	for (uint32_t b = 0; b < ach_menu_list->num_buckets && idx < total_achievements; b++) {
		const rc_client_achievement_bucket_t* bucket = &ach_menu_list->buckets[b];
		for (uint32_t a = 0; a < bucket->num_achievements && idx < total_achievements; a++) {
			all_achievements[idx++] = bucket->achievements[a];
		}
	}

	// Sort achievements according to settings
	ach_sort_achievements(all_achievements, total_achievements);

	// Custom menu loop with X (mute) and Y (filter) support
	int dirty = 1;
	int filter_dirty = 1;  // Rebuild filtered list when this is set
	int show_menu = 1;
	int selected = 0;
	int start = 0;
	int max_visible = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE);
	
	// Allocate filtered array once (max size = all achievements)
	const rc_client_achievement_t** filtered = calloc(total_achievements, sizeof(rc_client_achievement_t*));
	int filtered_count = 0;
	
	// Hide "Unknown Emulator" warning (ID 101000001) when hardcore mode is disabled.
	// Show it when enabled so users understand why they only get softcore unlocks.
	// Note: We intentionally show "Unsupported Game Version" so users know to find a supported ROM.
	bool hide_unknown_emulator = !CFG_getRAHardcoreMode();

	while (show_menu) {
		GFX_startFrame();
		PAD_poll();

		// Rebuild filtered list only when filter changes
		if (filter_dirty) {
			filtered_count = 0;
			for (int j = 0; j < total_achievements; j++) {
				// Skip "Unknown Emulator" warning when hardcore mode is disabled
				if (hide_unknown_emulator && all_achievements[j]->id == 101000001) {
					continue;
				}
				if (!ach_filter_locked_only || !all_achievements[j]->unlocked) {
					filtered[filtered_count++] = all_achievements[j];
				}
			}

			if (filtered_count == 0) {
				if (ach_filter_locked_only) {
					// No locked achievements when filter is on - revert filter
					ach_filter_locked_only = false;
					continue;  // Will rebuild with filter off
				}
				// No achievements at all after filtering - exit menu
				free(all_achievements);
				free(filtered);
				RA_destroyAchievementList(ach_menu_list);
				ach_menu_list = NULL;
				ach_menu_achievements = NULL;
				ach_menu_count = 0;
				Menu_message(T("ach.none_found"), (char*[]){"B",T("btn.back"), NULL});
				return MENU_CALLBACK_NOP;
			}

			// Store for detail view
			ach_menu_achievements = filtered;
			ach_menu_count = filtered_count;

			// Reset scroll position when filter changes
			if (selected >= filtered_count) selected = filtered_count - 1;
			if (selected < 0) selected = 0;
			start = 0;

			filter_dirty = 0;
			dirty = 1;
		}

		int end = MIN(start + max_visible, filtered_count);

		if (PAD_justRepeated(BTN_UP)) {
			selected--;
			if (selected < 0) {
				selected = filtered_count - 1;
				start = MAX(0, filtered_count - max_visible);
			} else if (selected < start) {
				start--;
			}
			dirty = 1;
		} else if (PAD_justRepeated(BTN_DOWN)) {
			selected++;
			if (selected >= filtered_count) {
				selected = 0;
				start = 0;
			} else if (selected >= end) {
				start++;
			}
			dirty = 1;
		} else if (PAD_justRepeated(BTN_LEFT)) {
			// Page up (move up by max_visible items)
			selected -= max_visible;
			if (selected < 0) {
				selected = 0;
				start = 0;
			} else {
				start = selected;
			}
			dirty = 1;
		} else if (PAD_justRepeated(BTN_RIGHT)) {
			// Page down (move down by max_visible items)
			selected += max_visible;
			if (selected >= filtered_count) {
				selected = filtered_count - 1;
				start = MAX(0, filtered_count - max_visible);
			} else {
				start = selected;
			}
			dirty = 1;
		} else if (PAD_justPressed(BTN_B)) {
			show_menu = 0;
		} else if (PAD_justPressed(BTN_A)) {
			// Show detail view (returns updated index after navigation)
			selected = OptionAchievements_showDetail(NULL, selected);
			// Adjust scroll position if needed
			if (selected < start) {
				start = selected;
			} else if (selected >= start + max_visible) {
				start = selected - max_visible + 1;
			}
			dirty = 1;
		} else if (PAD_justPressed(BTN_X)) {
			// Toggle mute for selected achievement
			if (filtered_count > 0) {
				const rc_client_achievement_t* ach = filtered[selected];
				RA_toggleAchievementMute(ach->id);
				dirty = 1;
			}
		} else if (PAD_justPressed(BTN_Y)) {
			// Toggle filter: All <-> Locked Only
			ach_filter_locked_only = !ach_filter_locked_only;
			selected = 0;
			start = 0;
			filter_dirty = 1;
		}

		if (dirty) {
			end = MIN(start + max_visible, filtered_count);
			
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, 0);

			// Layout constants matching MENU_FIXED style
			int mw = screen->w - SCALE1(PADDING * 2);
			int ox = SCALE1(PADDING);
			int row_height = SCALE1(BUTTON_SIZE);
			int selected_row = selected - start;
			int opt_pad = SCALE1(8);  // Local option padding since OPTION_PADDING defined later

			// Status text at top, aligned with hardware pill (not part of centered content)
			char status_text[64];
			snprintf(status_text, sizeof(status_text), "%u/%u unlocked", unlocked, total);
			SDL_Surface* status_surface = TTF_RenderUTF8_Blended(font.tiny, status_text, COLOR_WHITE);
			SDL_BlitSurface(status_surface, NULL, screen, &(SDL_Rect){
				(screen->w - status_surface->w) / 2,
				SCALE1(PADDING) + (SCALE1(PILL_SIZE) - status_surface->h) / 2  // Vertically centered with pill
			});
			SDL_FreeSurface(status_surface);

			// Calculate vertical centering for list only
			int top_margin = SCALE1(PADDING + PILL_SIZE);  // Below hardware pill / status text
			int bottom_margin = SCALE1(PADDING + PILL_SIZE);  // Above button hints
			int available_height = screen->h - top_margin - bottom_margin;
			
			// Content: just the visible rows
			int visible_rows = MIN(end - start, filtered_count);
			int list_height = visible_rows * row_height;
			
			// Center list vertically in available space
			int oy = top_margin + (available_height - list_height) / 2;

			// Draw achievement list rows
			for (int j = start, row = 0; j < end && j < filtered_count; j++, row++) {
				const rc_client_achievement_t* ach = filtered[j];
				bool is_muted = RA_isAchievementMuted(ach->id);
				bool is_offline_pending = RA_isAchievementOfflinePending(ach->id);
				bool is_selected = (row == selected_row);
				SDL_Color text_color = COLOR_WHITE;

				if (is_selected) {
					// Gray pill background for full row width (like MENU_FIXED selected)
					GFX_blitPillLight(ASSET_BUTTON, screen, &(SDL_Rect){
						ox, oy + SCALE1(row * BUTTON_SIZE), mw, row_height
					});
				}

				// Draw ">" on the right side (always white)
				SDL_Surface* arrow = TTF_RenderUTF8_Blended(font.small, ">", COLOR_WHITE);
				SDL_BlitSurface(arrow, NULL, screen, &(SDL_Rect){
					ox + mw - arrow->w - opt_pad,
					oy + SCALE1((row * BUTTON_SIZE) + 3)
				});
				SDL_FreeSurface(arrow);

				if (is_selected) {
					// White pill for the text area with icon (like MENU_FIXED selected text pill)
					// Calculate width needed for: badge + spacing + [mute icon] + [wifi icon] + title + padding
					int badge_display_size = SCALE1(BUTTON_SIZE - 4);  // Badge sized to fit in row
					int title_width = 0;
					TTF_SizeUTF8(font.small, ach->title, &title_width, NULL);
					int mute_width = 0;
					if (is_muted) {
						mute_width = SCALE1(10) + SCALE1(4);  // volume-mute icon + gap
					}
					int offline_width = 0;
					if (is_offline_pending) {
						offline_width = SCALE1(12) + SCALE1(4);  // wifi-off icon + gap
					}
					int pill_width = opt_pad + badge_display_size + SCALE1(6) + mute_width + offline_width + title_width + opt_pad;
					
					GFX_blitPillDark(ASSET_BUTTON, screen, &(SDL_Rect){
						ox, oy + SCALE1(row * BUTTON_SIZE), pill_width, row_height
					});
					text_color = uintToColour(THEME_COLOR5_255);

					// Badge icon inside the white pill
					SDL_Surface* badge = RA_Badges_get(ach->badge_name, !ach->unlocked);
					if (badge) {
						SDL_Rect badge_src = {0, 0, badge->w, badge->h};
						SDL_Rect badge_dst = {
							ox + opt_pad,
							oy + SCALE1(row * BUTTON_SIZE) + (row_height - badge_display_size) / 2,
							badge_display_size, badge_display_size
						};
						SDL_BlitScaled(badge, &badge_src, screen, &badge_dst);
					}

					int text_x = ox + opt_pad + badge_display_size + SCALE1(6);

					// Mute icon prefix (volume-mute icon, cropped to match text height)
					if (is_muted) {
						int mute_icon_h = SCALE1(12);
						int mute_y = oy + SCALE1(row * BUTTON_SIZE) + (row_height - mute_icon_h) / 2;
						SDL_Rect mute_src = {0, SCALE1(4), SCALE1(10), SCALE1(12)};
						GFX_blitAssetColor(ASSET_VOLUME_MUTE, &mute_src, screen,
						                   &(SDL_Rect){text_x, mute_y}, THEME_COLOR5_255);
						text_x += mute_width;
					}

					// Offline pending icon prefix (wifi-off icon before title)
					if (is_offline_pending) {
						int wifi_size = SCALE1(12);
						int wifi_y = oy + SCALE1(row * BUTTON_SIZE) + (row_height - wifi_size) / 2;
						GFX_blitAssetColor(ASSET_WIFI_OFF, NULL, screen,
						                   &(SDL_Rect){text_x, wifi_y}, THEME_COLOR5_255);
						text_x += offline_width;
					}

					// Title text
					SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.small, ach->title, text_color);
					SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){
						text_x,
						oy + SCALE1((row * BUTTON_SIZE) + 1)
					});
					SDL_FreeSurface(title_text);
				} else {
					// Unselected row - just badge + title + indicators, no pills
					int badge_display_size = SCALE1(BUTTON_SIZE - 4);
					
					// Badge icon
					SDL_Surface* badge = RA_Badges_get(ach->badge_name, !ach->unlocked);
					if (badge) {
						SDL_Rect badge_src = {0, 0, badge->w, badge->h};
						SDL_Rect badge_dst = {
							ox + opt_pad,
							oy + SCALE1(row * BUTTON_SIZE) + (row_height - badge_display_size) / 2,
							badge_display_size, badge_display_size
						};
						SDL_BlitScaled(badge, &badge_src, screen, &badge_dst);
					}

					int text_x = ox + opt_pad + badge_display_size + SCALE1(6);

					// Mute icon prefix (volume-mute icon, cropped to match text height)
					if (is_muted) {
						int mute_icon_h = SCALE1(12);
						int mute_y = oy + SCALE1(row * BUTTON_SIZE) + (row_height - mute_icon_h) / 2;
						SDL_Rect mute_src = {0, SCALE1(4), SCALE1(10), SCALE1(12)};
						GFX_blitAssetColor(ASSET_VOLUME_MUTE, &mute_src, screen,
						                   &(SDL_Rect){text_x, mute_y}, RGB_WHITE);
						text_x += SCALE1(10) + SCALE1(4);
					}

					// Offline pending icon prefix (wifi-off icon before title)
					if (is_offline_pending) {
						int wifi_size = SCALE1(12);
						int wifi_y = oy + SCALE1(row * BUTTON_SIZE) + (row_height - wifi_size) / 2;
						GFX_blitAssetColor(ASSET_WIFI_OFF, NULL, screen,
						                   &(SDL_Rect){text_x, wifi_y}, RGB_WHITE);
						text_x += wifi_size + SCALE1(4);
					}

					// Title text (white for unselected)
					SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.small, ach->title, COLOR_WHITE);
					SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){
						text_x,
						oy + SCALE1((row * BUTTON_SIZE) + 1)
					});
					SDL_FreeSurface(title_text);
				}
			}

			// Button hints at bottom with dynamic Y and X button text
			int selected_muted = (filtered_count > 0) ? RA_isAchievementMuted(filtered[selected]->id) : 0;
			char* hints[] = {"Y", ach_filter_locked_only ? T("btn.show_all") : T("btn.show_locked"), "X", selected_muted ? T("btn.unmute") : T("btn.mute"), NULL};
			GFX_blitButtonGroup(hints, 0, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
	}

	// Cleanup
	free(all_achievements);
	free(filtered);  // This was assigned to ach_menu_achievements
	ach_menu_achievements = NULL;
	ach_menu_count = 0;
	if (ach_menu_list) {
		RA_destroyAchievementList(ach_menu_list);
		ach_menu_list = NULL;
	}

	return MENU_CALLBACK_NOP;
}

static MenuList options_menu = {
	.type = MENU_LIST,
	.items = (MenuItem[]) {
		{NULL, "NextUI (" BUILD_DATE " " BUILD_HASH ")",.on_confirm=OptionFrontend_openMenu},
		{NULL,.on_confirm=OptionEmulator_openMenu},
		{NULL,.on_confirm=OptionShaders_openMenu},
		{NULL,.on_confirm=OptionCheats_openMenu},
		{NULL,.on_confirm=OptionControls_openMenu},
		{NULL,.on_confirm=OptionShortcuts_openMenu},
		{NULL,.on_confirm=OptionAchievements_openMenu},
		{NULL,.on_confirm=OptionSaveChanges_openMenu},
		{NULL},
		{NULL},
	}
};

// Track the index of Save Changes menu item (changes based on RA visibility)
static int save_changes_index = 7;

// Update options menu visibility based on RA enable state
static void Options_updateVisibility(void) {
	// Always (re)apply translated labels for the fixed entries (0..5).
	options_menu.items[0].name = T("menu.opt.frontend");
	options_menu.items[1].name = T("menu.opt.emulator");
	options_menu.items[2].name = T("menu.opt.shaders");
	options_menu.items[3].name = T("menu.opt.cheats");
	options_menu.items[4].name = T("menu.opt.controls");
	options_menu.items[5].name = T("menu.opt.shortcuts");

	if (CFG_getRAEnable()) {
		// RA enabled: show Achievements at index 6, Save Changes at index 7
		options_menu.items[6].name = T("menu.opt.achievements");
		options_menu.items[6].on_confirm = OptionAchievements_openMenu;
		options_menu.items[7].name = T("menu.opt.save_changes");
		options_menu.items[7].on_confirm = OptionSaveChanges_openMenu;
		save_changes_index = 7;
	} else {
		// RA disabled: hide Achievements, move Save Changes to index 6
		options_menu.items[6].name = T("menu.opt.save_changes");
		options_menu.items[6].desc = NULL;
		options_menu.items[6].on_confirm = OptionSaveChanges_openMenu;
		options_menu.items[7].name = NULL;
		options_menu.items[7].on_confirm = NULL;
		save_changes_index = 6;
	}
}

void OptionSaveChanges_updateDesc(void) {
	options_menu.items[save_changes_index].desc = getSaveDesc();
}

// Update the Achievements menu item to show count (only when RA enabled)
static char ach_desc_buffer[64] = {0};
void OptionAchievements_updateDesc(void) {
	if (!CFG_getRAEnable()) return;
	
	if (RA_isGameLoaded()) {
		uint32_t unlocked, total;
		RA_getAchievementSummary(&unlocked, &total);
		if (total > 0) {
			snprintf(ach_desc_buffer, sizeof(ach_desc_buffer), "%u / %u unlocked", unlocked, total);
			options_menu.items[6].desc = ach_desc_buffer;
			return;
		}
	}
	options_menu.items[6].desc = NULL;
}

#define OPTION_PADDING 8
bool getAlias(char* path, char* alias) {
	bool is_alias = false;
	// LOG_info("alias path: %s\n", path);
	char* tmp;
	char map_path[256];
	strcpy(map_path, path);
	tmp = strrchr(map_path, '/');
	if (tmp) {
		tmp += 1;
		strcpy(tmp, "map.txt");
		// LOG_info("map_path: %s\n", map_path);
	}
	char* file_name = strrchr(path,'/');
	if (file_name) file_name += 1;
	// LOG_info("file_name: %s\n", file_name);
	
	if (exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines
			
				tmp = strchr(line,'\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp+1;
					if (exactMatch(file_name,key)) {
						strcpy(alias, value);
						is_alias = true;
						break;
					}
				}
			}
			fclose(file);
		}
	}
	return is_alias;
}

int Menu_options(MenuList* list) {
	MenuItem* items = list->items;
	int type = list->type;

	int dirty = 1;
	int show_options = 1;
	int show_settings = 0;
	int await_input = 0;
	
	// dependent on option list offset top and bottom, eg. the gray triangles
	int max_visible_options = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE); // 7 for 480, 10 for 720
	
	int count;
	for (count=0; items[count].name; count++);
	int selected = 0;
	int start = 0;
	int end = MIN(count,max_visible_options);
	int visible_rows = end;
	
	OptionSaveChanges_updateDesc();
	OptionAchievements_updateDesc();
	
	int defer_menu = false;
	while (show_options) {
		if (await_input) {
			defer_menu = true;
			list->on_confirm(list, selected);
			
			selected += 1;
			if (selected>=count) {
				selected = 0;
				start = 0;
				end = visible_rows;
			}
			else if (selected>=end) {
				start += 1;
				end += 1;
			}
			dirty = 1;
			await_input = false;
		}
		
		GFX_startFrame();
		PAD_poll();
		if (PAD_justRepeated(BTN_UP)) {
			selected -= 1;
			if (selected<0) {
				selected = count - 1;
				start = MAX(0,count - max_visible_options);
				end = count;
			}
			else if (selected<start) {
				start -= 1;
				end -= 1;
			}
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			selected += 1;
			if (selected>=count) {
				selected = 0;
				start = 0;
				end = visible_rows;
			}
			else if (selected>=end) {
				start += 1;
				end += 1;
			}
			dirty = 1;
		}
		else {
			MenuItem* item = &items[selected];
			if (item->values && item->values!=button_labels) { // not an input binding
				if (PAD_justRepeated(BTN_LEFT)) {
					if (item->value>0) item->value -= 1;
					else {
						int j;
						for (j=0; item->values[j]; j++);
						item->value = j - 1;
					}
				
					if (item->on_change) item->on_change(list, selected);
					else if (list->on_change) list->on_change(list, selected);
				
					dirty = 1;
				}
				else if (PAD_justRepeated(BTN_RIGHT)) {
					// first check if its not out of bounds already
					int i = 0;
					while (item->values[i]) i++; 
					if (item->value >= i) item->value = 0;
				
					if (item->values[item->value+1]) item->value += 1;
					else item->value = 0;
				
					if (item->on_change) item->on_change(list, selected);
					else if (list->on_change) list->on_change(list, selected);
				
					dirty = 1;
				}
			}
		}
		
		// uint32_t now = SDL_GetTicks();
		if (PAD_justPressed(BTN_B)) { // || PAD_tappedMenu(now)
			show_options = 0;
		}
		else if (PAD_justPressed(BTN_A)) {
			MenuItem* item = &items[selected];
			int result = MENU_CALLBACK_NOP;
			if (item->on_confirm) result = item->on_confirm(list, selected); // item-specific action, eg. Save for all games
			else if (item->submenu) result = Menu_options(item->submenu); // drill down, eg. main options menu
			// TODO: is there a way to defer on_confirm for MENU_INPUT so we can clear the currently set value to indicate it is awaiting input? 
			// eg. set a flag to call on_confirm at the beginning of the next frame?
			else if (list->on_confirm) {
				if (item->values==button_labels) await_input = 1; // button binding
				else result = list->on_confirm(list, selected); // list-specific action, eg. show item detail view or input binding
			}
			if (result==MENU_CALLBACK_EXIT) show_options = 0;
			else {
				if (result==MENU_CALLBACK_NEXT_ITEM) {
					// copied from PAD_justRepeated(BTN_DOWN) above
					selected += 1;
					if (selected>=count) {
						selected = 0;
						start = 0;
						end = visible_rows;
					}
					else if (selected>=end) {
						start += 1;
						end += 1;
					}
				}
				dirty = 1;
			}
		}
		else if (type==MENU_INPUT) {
			if (PAD_justPressed(BTN_X)) {
				MenuItem* item = &items[selected];
				item->value = 0;
				
				if (item->on_change) item->on_change(list, selected);
				else if (list->on_change) list->on_change(list, selected);
				
				// copied from PAD_justRepeated(BTN_DOWN) above
				selected += 1;
				if (selected>=count) {
					selected = 0;
					start = 0;
					end = visible_rows;
				}
				else if (selected>=end) {
					start += 1;
					end += 1;
				}
				dirty = 1;
			}
		}
		
		if (!defer_menu) PWR_update(&dirty, &show_settings, Menu_beforeSleep, Menu_afterSleep);
		
		if (defer_menu && PAD_justReleased(BTN_MENU)) defer_menu = false;
		
		GFX_clear(screen);
		GFX_blitHardwareGroup(screen, show_settings);
		
		char* desc = NULL;
		SDL_Surface* text;

		if (type==MENU_LIST) {
			int mw = list->max_width;
			if (!mw) {
				// get the width of the widest item
				for (int i=0; i<count; i++) {
					MenuItem* item = &items[i];
					int w = 0;
					TTF_SizeUTF8(font.small, item->name, &w, NULL);
					w += SCALE1(OPTION_PADDING*2);
					if (w>mw) mw = w;
				}
				// cache the result
				list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING *2));
			}
			
			int ox = (screen->w - mw) / 2;
			int oy = SCALE1(PADDING + PILL_SIZE);
			int selected_row = selected - start;
			for (int i=start,j=0; i<end; i++,j++) {
				MenuItem* item = &items[i];
				SDL_Color text_color = COLOR_WHITE;
				// int ox = (screen->w - w) / 2; // if we're centering these (but I don't think we should after seeing it)
				if (j==selected_row) {
					// move out of conditional if centering
					int w = 0;
					TTF_SizeUTF8(font.small, item->name, &w, NULL);
					w += SCALE1(OPTION_PADDING*2);
					
					GFX_blitPillDark(ASSET_BUTTON, screen, &(SDL_Rect){
						ox,
						oy+SCALE1(j*BUTTON_SIZE),
						w,
						SCALE1(BUTTON_SIZE)
					});
					text_color = uintToColour(THEME_COLOR5_255);
					
					if (item->desc) desc = item->desc;
				}
				text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
					ox+SCALE1(OPTION_PADDING),
					oy+SCALE1((j*BUTTON_SIZE)+1)
				});
				SDL_FreeSurface(text);
			}
		}
		else if (type==MENU_FIXED) {
			// NOTE: no need to calculate max width
			int mw = screen->w - SCALE1(PADDING*2);
			// int lw,rw;
			// lw = rw = mw / 2;
			int ox,oy;
			ox = oy = SCALE1(PADDING);
			oy += SCALE1(PILL_SIZE);
			
			int selected_row = selected - start;
			for (int i=start,j=0; i<end; i++,j++) {
				MenuItem* item = &items[i];
				SDL_Color text_color = COLOR_WHITE;

				if (j==selected_row) {
					// gray pill
					GFX_blitPillLight(ASSET_BUTTON, screen, &(SDL_Rect){
						ox,
						oy+SCALE1(j*BUTTON_SIZE),
						mw,
						SCALE1(BUTTON_SIZE)
					});
				}
				
				if (item->values == NULL) {
					// This is a navigation item, used to displayed a specific category
					text = TTF_RenderUTF8_Blended(font.small, ">", COLOR_WHITE); // always white
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
						ox + mw - text->w - SCALE1(OPTION_PADDING),
						oy+SCALE1((j*BUTTON_SIZE)+3)
					});
					SDL_FreeSurface(text);
				}
				else {
					if (item->value>=0) {
						int count = 0;
						while ( item->values && item->values[count]) count++;
						if (item->value >= 0 && item->value < count) {
							const char *str = item->values[item->value];
							text = TTF_RenderUTF8_Blended(font.tiny, str ? str : T("val.none"), str ? COLOR_WHITE : COLOR_GRAY); // always white
							if (text) {
								SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
									ox + mw - text->w - SCALE1(OPTION_PADDING),
									oy+SCALE1((j*BUTTON_SIZE)+3)
								});
								SDL_FreeSurface(text);
							}
						}
					}
				}
				
				// TODO: blit a black pill on unselected rows (to cover longer item->values?) or truncate longer item->values?
				if (j==selected_row) {
					// white pill
					int w = 0;
					TTF_SizeUTF8(font.small, item->name, &w, NULL);
					w += SCALE1(OPTION_PADDING*2);
					GFX_blitPillDark(ASSET_BUTTON, screen, &(SDL_Rect){
						ox,
						oy+SCALE1(j*BUTTON_SIZE),
						w,
						SCALE1(BUTTON_SIZE)
					});
					text_color = uintToColour(THEME_COLOR5_255);
					
					if (item->desc) desc = item->desc;
				}
				text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
					ox+SCALE1(OPTION_PADDING),
					oy+SCALE1((j*BUTTON_SIZE)+1)
				});
				SDL_FreeSurface(text);
			}
		}
		else if (type==MENU_VAR || type==MENU_INPUT) {
			int mw = list->max_width;
			if (!mw) {
				// get the width of the widest row
				int mrw = 0;
				for (int i=0; i<count; i++) {
					MenuItem* item = &items[i];
					int w = 0;
					int lw = 0;
					int rw = 0;
					TTF_SizeUTF8(font.small, item->name, &lw, NULL);
					// every value list in an input table is the same
					// so only calculate rw for the first item...
					if (!mrw || type!=MENU_INPUT) {
						if(item->values) {
							for (int j=0; item->values[j]; j++) {
								TTF_SizeUTF8(font.tiny, item->values[j], &rw, NULL);
								if (lw+rw>w) w = lw+rw;
								if (rw>mrw) mrw = rw;
							}
						}
					}
					else {
						w = lw + mrw;
					}
					w += SCALE1(OPTION_PADDING*4);
					if (w>mw) mw = w;
				}
				fflush(stdout);
				// cache the result
				list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING *2));
			}
			int ox = (screen->w - mw) / 2;
			int oy = SCALE1(PADDING + PILL_SIZE);
			int selected_row = selected - start;
			for (int i=start,j=0; i<end; i++,j++) {
				MenuItem* item = &items[i];
				SDL_Color text_color = COLOR_WHITE;
				

				if (j==selected_row) {
					// gray pill
					GFX_blitPillLight(ASSET_BUTTON, screen, &(SDL_Rect){
						ox,
						oy+SCALE1(j*BUTTON_SIZE),
						mw,
						SCALE1(BUTTON_SIZE)
					});
					
					// white pill
					int w = 0;
					TTF_SizeUTF8(font.small, item->name, &w, NULL);
					w += SCALE1(OPTION_PADDING*2);
					GFX_blitPillDark(ASSET_BUTTON, screen, &(SDL_Rect){
						ox,
						oy+SCALE1(j*BUTTON_SIZE),
						w,
						SCALE1(BUTTON_SIZE)
					});
					text_color = uintToColour(THEME_COLOR5_255);
					
					if (item->desc) desc = item->desc;
				}
				text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
					ox+SCALE1(OPTION_PADDING),
					oy+SCALE1((j*BUTTON_SIZE)+1)
				});
				SDL_FreeSurface(text);
				
				if (await_input && j==selected_row) {
					// buh
				}
				else if (item->value>=0) {
					int count = 0;
					while ( item->values && item->values[count]) count++;
					if (item->value >= 0 && item->value < count) {
						text = TTF_RenderUTF8_Blended(font.tiny, item->values[item->value], COLOR_WHITE); // always white
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
							ox + mw - text->w - SCALE1(OPTION_PADDING),
							oy+SCALE1((j*BUTTON_SIZE)+3)
						});
						SDL_FreeSurface(text);
					}
				}
			}
		}
		
		if (count>max_visible_options) {
			#define SCROLL_WIDTH 24
			#define SCROLL_HEIGHT 4
			int ox = (screen->w - SCALE1(SCROLL_WIDTH))/2;
			int oy = SCALE1((PILL_SIZE - SCROLL_HEIGHT) / 2);
			if (start>0) GFX_blitAsset(ASSET_SCROLL_UP,   NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING) + oy});
			if (end<count) GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE) + oy});
		}
		
		if (!desc && list->desc) desc = list->desc;
		
		if (desc) {
			int w,h;
			GFX_sizeText(font.tiny, desc, SCALE1(12), &w,&h);
			GFX_blitText(font.tiny, desc, SCALE1(12), COLOR_WHITE, screen, &(SDL_Rect){
				(screen->w - w) / 2,
				screen->h - SCALE1(PADDING) - h,
				w,h
			});
		}
		
		GFX_flip(screen);
		dirty = 0;
		
		hdmimon();
	}
	
	// GFX_clearAll();
	// GFX_flip(screen);
	
	return 0;
}

static void Menu_scale(SDL_Surface* src, SDL_Surface* dst) {
	// LOG_info("Menu_scale src: %ix%i dst: %ix%i\n", src->w,src->h,dst->w,dst->h);
	
	uint16_t* s = src->pixels;
	uint16_t* d = dst->pixels;
	
	int sw = src->w;
	int sh = src->h;
	int sp = src->pitch / FIXED_BPP;
	
	int dw = dst->w;
	int dh = dst->h;
	int dp = dst->pitch / FIXED_BPP;
	
	int rx = 0;
	int ry = 0;
	int rw = dw;
	int rh = dh;
	
	int scaling = screen_scaling;
	if (scaling==SCALE_CROPPED && DEVICE_WIDTH==HDMI_WIDTH) {
		scaling = SCALE_NATIVE;
	}
	if (scaling==SCALE_NATIVE) {
		// LOG_info("native\n");
		
		rx = renderer.dst_x;
		ry = renderer.dst_y;
		rw = renderer.src_w;
		rh = renderer.src_h;
		if (renderer.scale) {
			// LOG_info("scale: %i\n", renderer.scale);
			rw *= renderer.scale;
			rh *= renderer.scale;
		}
		else {
			// LOG_info("forced crop\n"); // eg. fc on nano, vb on smart
			rw -= renderer.src_x * 2;
			rh -= renderer.src_y * 2;
			sw = rw;
			sh = rh;
		}
		
		if (dw==DEVICE_WIDTH/2) {
			// LOG_info("halve\n");
			rx /= 2;
			ry /= 2;
			rw /= 2;
			rh /= 2;
		}
	}
	else if (scaling==SCALE_CROPPED) {
		// LOG_info("cropped\n");
		sw -= renderer.src_x * 2;
		sh -= renderer.src_y * 2;

		rx = renderer.dst_x;
		ry = renderer.dst_y;
		rw = sw * renderer.scale;
		rh = sh * renderer.scale;
		
		if (dw==DEVICE_WIDTH/2) {
			// LOG_info("halve\n");
			rx /= 2;
			ry /= 2;
			rw /= 2;
			rh /= 2;
		}
	}
	
	if (scaling==SCALE_ASPECT || rw>dw || rh>dh) {
		// LOG_info("aspect\n");
		double fixed_aspect_ratio = ((double)DEVICE_WIDTH) / DEVICE_HEIGHT;
		int core_aspect = core.aspect_ratio * 1000;
		int fixed_aspect = fixed_aspect_ratio * 1000;
		
		if (core_aspect>fixed_aspect) {
			// LOG_info("letterbox\n");
			rw = dw;
			rh = rw / core.aspect_ratio;
			rh += rh%2;
		}
		else if (core_aspect<fixed_aspect) {
			// LOG_info("pillarbox\n");
			rh = dh;
			rw = rh * core.aspect_ratio;
			rw += rw%2;
			rw = (rw/8)*8; // probably necessary here since we're not scaling by an integer
		}
		else {
			// LOG_info("perfect match\n");
			rw = dw;
			rh = dh;
		}
		
		rx = (dw - rw) / 2;
		ry = (dh - rh) / 2;
	}
	
	// LOG_info("Menu_scale (r): %i,%i %ix%i\n",rx,ry,rw,rh);
	// LOG_info("offset: %i,%i\n", renderer.src_x, renderer.src_y);

	// dumb nearest neighbor scaling
	int mx = (sw << 16) / rw;
	int my = (sh << 16) / rh;
	int ox = (renderer.src_x << 16);
	int sx = ox;
	int sy = (renderer.src_y << 16);
	int lr = -1;
	int sr = 0;
	int dr = ry * dp;
	int cp = dp * FIXED_BPP;
	
	// LOG_info("Menu_scale (s): %i,%i %ix%i\n",sx,sy,sw,sh);
	// LOG_info("mx:%i my:%i sx>>16:%i sy>>16:%i\n",mx,my,((sx+mx) >> 16),((sy+my) >> 16));

	for (int dy=0; dy<rh; dy++) {
		sx = ox;
		sr = (sy >> 16) * sp;
		if (sr==lr) {
			memcpy(d+dr,d+dr-dp,cp);
		}
		else {
	        for (int dx=0; dx<rw; dx++) {
	            d[dr + rx + dx] = s[sr + (sx >> 16)];
				sx += mx;
	        }
		}
		lr = sr;
		sy += my;
		dr += dp;
    }
	
	// LOG_info("successful\n");
}

void Menu_initState(void) {
	if (exists(menu.slot_path)) menu.slot = getInt(menu.slot_path);
	if (menu.slot==8) menu.slot = 0;
	
	menu.save_exists = 0;
	menu.preview_exists = 0;
}
static void Menu_updateState(void) {
	// LOG_info("Menu_updateState\n");

	int last_slot = state_slot;
	state_slot = menu.slot;

	char save_path[256];
	State_getPath(save_path);

	state_slot = last_slot;

	// always sanitized/outer name, to keep main UI from having to inspect archives
	sprintf(menu.bmp_path, "%s/%s.%d.bmp", menu.minui_dir, game.name, menu.slot);
	sprintf(menu.txt_path, "%s/%s.%d.txt", menu.minui_dir, game.name, menu.slot);
	
	menu.save_exists = exists(save_path);
	menu.preview_exists = menu.save_exists && exists(menu.bmp_path);

	// LOG_info("save_path: %s (%i)\n", save_path, menu.save_exists);
	// LOG_info("bmp_path: %s txt_path: %s (%i)\n", menu.bmp_path, menu.txt_path, menu.preview_exists);
}

typedef struct {
    char* pixels;
    char* path;
	int w;
	int h;
} SaveImageArgs;

int save_screenshot_thread(void* data) {

    SaveImageArgs* args = (SaveImageArgs*)data;
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		args->pixels, args->w, args->h, 32, args->w * 4, SDL_PIXELFORMAT_ABGR8888
	);
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(rawSurface, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(rawSurface);

    SDL_RWops* rw = SDL_RWFromFile(args->path, "wb");
    if (!rw) {
        SDL_Log("Failed to open file for writing: %s", SDL_GetError());
    } else {
        if (IMG_SavePNG_RW(converted, rw, 1) != 0) {
            SDL_Log("Failed to save PNG: %s", SDL_GetError());
        }
    }
	LOG_info("saved screenshot\n");
	SDL_FreeSurface(converted);
    free(args->path);
    free(args->pixels);
    free(args);
    return 0;
}
SDL_Thread* screenshotsavethread;
void Menu_screenshot(void) {
	LOG_info("Menu_screenshot\n");

	char rom_name[256];
	getDisplayName(game.alt_name, rom_name);
	getAlias(game.path, rom_name);

	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char buffer[100];
	strftime(buffer, sizeof(buffer), "%Y-%m-%d-%H-%M-%S", t);

	// make sure this actually exists
	mkdir(SDCARD_PATH "/Screenshots", 0755);

	char png_path[256];
	sprintf(png_path, SDCARD_PATH "/Screenshots/%s.%s.png", rom_name, buffer);
	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);
	SaveImageArgs* args = malloc(sizeof(SaveImageArgs));
	args->pixels = pixels;
	args->w = cw;
	args->h = ch;
	args->path = SDL_strdup(png_path);
	SDL_WaitThread(screenshotsavethread, NULL);
	screenshotsavethread = SDL_CreateThread(save_screenshot_thread, "SaveScreenshotThread", args);
	
	// Show notification if enabled
	if (CFG_getNotifyScreenshot()) {
		Notification_push(NOTIFICATION_SETTING, T("menu.screenshot_saved"), NULL);
	}
}
void Menu_saveState(void) {
	// LOG_info("Menu_saveState\n");
	Menu_updateState();
	
	if (menu.total_discs) {
		char* disc_path = menu.disc_paths[menu.disc];
		putFile(menu.txt_path, disc_path + strlen(menu.base_path));
	}
	
	// if already in menu use menu.bitmap instead for saving screenshots otherwise create new one on the fly
	if (newScreenshot) {
		int cw, ch;
		unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);
		SaveImageArgs* args = malloc(sizeof(SaveImageArgs));
		args->pixels = pixels;
		args->w = cw;
		args->h = ch;
		args->path = SDL_strdup(menu.bmp_path); 
		SDL_WaitThread(screenshotsavethread, NULL);
		screenshotsavethread = SDL_CreateThread(save_screenshot_thread, "SaveScreenshotThread", args);
		newScreenshot = 0;
	} else {
		SDL_RWops* rw = SDL_RWFromFile(menu.bmp_path, "wb");
		IMG_SavePNG_RW(menu.bitmap, rw,1);
		LOG_info("saved screenshot\n");
	}
	
	state_slot = menu.slot;
	putInt(menu.slot_path, menu.slot);
	int success = State_write();
	
	// Show notification if enabled
	if (CFG_getNotifyManualSave()) {
		char msg[NOTIFICATION_MAX_MESSAGE];
		// User-facing slots are 1-8 (internal 0-7)
		snprintf(msg, sizeof(msg), success ? "State Saved - Slot %d" : "Save Failed - Slot %d", menu.slot + 1);
		Notification_push(NOTIFICATION_SAVE_STATE, msg, NULL);
	}
}
void Menu_loadState(void) {
	Menu_updateState();

	if (menu.save_exists) {
		if (menu.total_discs) {
			char slot_disc_name[256];
			getFile(menu.txt_path, slot_disc_name, 256);

			char slot_disc_path[256];
			if (slot_disc_name[0]=='/') strcpy(slot_disc_path, slot_disc_name);
			else sprintf(slot_disc_path, "%s%s", menu.base_path, slot_disc_name);

			char* disc_path = menu.disc_paths[menu.disc];
			if (!exactMatch(slot_disc_path, disc_path)) {
				Game_changeDisc(slot_disc_path);
			}
		}

		state_slot = menu.slot;
		putInt(menu.slot_path, menu.slot);
		int success = State_read();
		Rewind_on_state_change();
		
		// Show notification if enabled
		if (CFG_getNotifyLoad()) {
			char msg[NOTIFICATION_MAX_MESSAGE];
			// User-facing slots are 1-8 (internal 0-7)
			snprintf(msg, sizeof(msg), success ? "State Loaded - Slot %d" : "Load Failed - Slot %d", menu.slot + 1);
			Notification_push(NOTIFICATION_LOAD_STATE, msg, NULL);
		}
	}
}

void Menu_loop(void) {

	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);
	
	renderer.dst = pixels;
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, cw, ch, 32, cw * 4, SDL_PIXELFORMAT_ABGR8888
	);
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(rawSurface, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(rawSurface);
	free(pixels); 

	menu.bitmap = converted;
	SDL_Surface* backing = SDL_CreateRGBSurfaceWithFormat(0,DEVICE_WIDTH,DEVICE_HEIGHT,32,SDL_PIXELFORMAT_ARGB8888); 
	

	SDL_Rect dst = {
		0,
		0,
		screen->w,
		screen->h
	};
	SDL_BlitScaled(menu.bitmap, NULL, backing, &dst);
	
	int restore_w = screen->w;
	int restore_h = screen->h;
	int restore_p = screen->pitch;
	if (restore_w!=DEVICE_WIDTH || restore_h!=DEVICE_HEIGHT) {
		screen = GFX_resize(DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);
	}

	SRAM_write();
	RTC_write();
	if (!HAS_POWER_BUTTON) PWR_enableSleep();

	GFX_setEffect(EFFECT_NONE);
	
	int rumble_strength = VIB_getStrength();
	VIB_setStrength(0);
	
	PWR_enableAutosleep();
	PAD_reset();
	
	// path and string things
	char* tmp;
	char rom_name[256]; // without extension or cruft
	getDisplayName(game.name, rom_name);
	getAlias(game.path, rom_name);
	
	int rom_disc = -1;
	char disc_name[16];
	if (menu.total_discs) {
		rom_disc = menu.disc;
		sprintf(disc_name, "Disc %i", menu.disc+1);
	}
		
	int selected = 0; // resets every launch
	Menu_initState();
	
	int status = STATUS_CONT; // TODO: no longer used?
	int show_setting = 0;
	int dirty = 1;
	int ignore_menu = 0;
	int menu_start = 0;
	SDL_Surface* preview = SDL_CreateRGBSurface(SDL_SWSURFACE,DEVICE_WIDTH/2,DEVICE_HEIGHT/2,32,RGBA_MASK_8888); // TODO: retain until changed?

	//set vid.blit to null for menu drawing no need for blitrender drawing
	GFX_clearShaders();
	while (show_menu) {

		GFX_startFrame();
		uint32_t now = SDL_GetTicks();

		PAD_poll();
		
		if (PAD_justPressed(BTN_UP)) {
			selected -= 1;
			if (selected<0) selected += MENU_ITEM_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_DOWN)) {
			selected += 1;
			if (selected>=MENU_ITEM_COUNT) selected -= MENU_ITEM_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_LEFT)) {
			if (menu.total_discs>1 && selected==ITEM_CONT) {
				menu.disc -= 1;
				if (menu.disc<0) menu.disc += menu.total_discs;
				dirty = 1;
				sprintf(disc_name, "Disc %i", menu.disc+1);
			}
			else if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				menu.slot -= 1;
				if (menu.slot<0) menu.slot += MENU_SLOT_COUNT;
				dirty = 1;
			}
		}
		else if (PAD_justPressed(BTN_RIGHT)) {
			if (menu.total_discs>1 && selected==ITEM_CONT) {
				menu.disc += 1;
				if (menu.disc==menu.total_discs) menu.disc -= menu.total_discs;
				dirty = 1;
				sprintf(disc_name, "Disc %i", menu.disc+1);
			}
			else if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				menu.slot += 1;
				if (menu.slot>=MENU_SLOT_COUNT) menu.slot -= MENU_SLOT_COUNT;
				dirty = 1;
			}
		}
		
		if (dirty && (selected==ITEM_SAVE || selected==ITEM_LOAD)) {
			Menu_updateState();
		}
		
		if (PAD_justPressed(BTN_B) || (BTN_WAKE!=BTN_MENU && PAD_tappedMenu(now))) {
			status = STATUS_CONT;
			show_menu = 0;
		}
		else if (PAD_justPressed(BTN_A)) {
			switch(selected) {
				case ITEM_CONT:
				if (menu.total_discs && rom_disc!=menu.disc) {
						status = STATUS_DISC;
						char* disc_path = menu.disc_paths[menu.disc];
						Game_changeDisc(disc_path);
					}
					else {
						status = STATUS_CONT;
					}
					show_menu = 0;
				break;
				
				case ITEM_SAVE: {
					Menu_saveState();
					status = STATUS_SAVE;
					show_menu = 0;
				}
				break;
				case ITEM_LOAD: {
					Menu_loadState();
					status = STATUS_LOAD;
					show_menu = 0;
				}
				break;
				case ITEM_OPTS: {
					if (simple_mode) {
						core.reset();
						status = STATUS_RESET;
						show_menu = 0;
					}
					else {
						int old_scaling = screen_scaling;
						Options_updateVisibility();
						Menu_options(&options_menu);
						if (screen_scaling!=old_scaling) {
							selectScaler(renderer.true_w,renderer.true_h,renderer.src_p);
						
							restore_w = screen->w;
							restore_h = screen->h;
							restore_p = screen->pitch;
							screen = GFX_resize(DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);
							SDL_Rect dst = {0, 0, DEVICE_WIDTH, DEVICE_HEIGHT};
							SDL_BlitScaled(menu.bitmap,NULL,backing,&dst);
						}
						dirty = 1;
					}
				}
				break;
				case ITEM_QUIT:
					status = STATUS_QUIT;
					show_menu = 0;
					quit = 1; // TODO: tmp?
				break;
			}
			if (!show_menu) break;
		}

		PWR_update(&dirty, &show_setting, Menu_beforeSleep, Menu_afterSleep);
		if(dirty) {
			GFX_clear(screen);

			GFX_drawOnLayer(menu.bitmap,0,0,DEVICE_WIDTH,DEVICE_HEIGHT,0.4f,1,0);
			

			int ox, oy;
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			int max_width = screen->w - SCALE1(PADDING * 2) - ow;
			
			char display_name[256];
			int text_width = GFX_truncateText(font.large, rom_name, display_name, max_width, SCALE1(BUTTON_PADDING*2));
			max_width = MIN(max_width, text_width);

			SDL_Surface* text;
			text = TTF_RenderUTF8_Blended(font.large, display_name, uintToColour(THEME_COLOR6_255));
			GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){
				SCALE1(PADDING),
				SCALE1(PADDING),
				max_width,
				SCALE1(PILL_SIZE)
			});
			SDL_BlitSurface(text, &(SDL_Rect){
				0,
				0,
				max_width-SCALE1(BUTTON_PADDING*2),
				text->h
			}, screen, &(SDL_Rect){
				SCALE1(PADDING+BUTTON_PADDING),
				SCALE1(PADDING+4)
			});
			SDL_FreeSurface(text);
			
			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ BTN_SLEEP==BTN_POWER?T("btn.power"):T("btn.menu"),T("btn.sleep"), NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B",T("btn.back"), "A",T("btn.okay"), NULL }, 1, screen, 1);
			
			// list
			oy = (((DEVICE_HEIGHT / FIXED_SCALE) - PADDING * 2) - (MENU_ITEM_COUNT * PILL_SIZE)) / 2;
			for (int i=0; i<MENU_ITEM_COUNT; i++) {
				char* item = menu.items[i];
				SDL_Color text_color = COLOR_WHITE;
				
				if (i==selected) {
					text_color = uintToColour(THEME_COLOR5_255);

					// disc change
					if (menu.total_discs>1 && i==ITEM_CONT) {				
						GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
							SCALE1(PADDING),
							SCALE1(oy + PADDING),
							screen->w - SCALE1(PADDING * 2),
							SCALE1(PILL_SIZE)
						});
						text = TTF_RenderUTF8_Blended(font.large, disc_name, text_color);
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
							screen->w - SCALE1(PADDING + BUTTON_PADDING) - text->w,
							SCALE1(oy + PADDING + 4)
						});
						SDL_FreeSurface(text);
					}
					
					TTF_SizeUTF8(font.large, item, &ow, NULL);
					ow += SCALE1(BUTTON_PADDING*2);
					
					// pill
					GFX_blitPillDark(ASSET_WHITE_PILL, screen, &(SDL_Rect){
						SCALE1(PADDING),
						SCALE1(oy + PADDING + (i * PILL_SIZE)),
						ow,
						SCALE1(PILL_SIZE)
					});
				}
			
				
				// text
				text = TTF_RenderUTF8_Blended(font.large, item, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
					SCALE1(PADDING + BUTTON_PADDING),
					SCALE1(oy + PADDING + (i * PILL_SIZE) + 4)
				});
				SDL_FreeSurface(text);
			}
			
			// slot preview
			if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				#define WINDOW_RADIUS 4 // TODO: this logic belongs in blitRect?
				#define PAGINATION_HEIGHT 6
				// unscaled
				int hw = DEVICE_WIDTH / 2;
				int hh = DEVICE_HEIGHT / 2;
				int pw = hw + SCALE1(WINDOW_RADIUS*2);
				int ph = hh + SCALE1(WINDOW_RADIUS*2 + PAGINATION_HEIGHT + WINDOW_RADIUS);
				ox = DEVICE_WIDTH - pw - SCALE1(PADDING);
				oy = (DEVICE_HEIGHT - ph) / 2;
				
				// window
				GFX_blitRect(ASSET_STATE_BG, screen, &(SDL_Rect){ox,oy,pw,ph});
				ox += SCALE1(WINDOW_RADIUS);
				oy += SCALE1(WINDOW_RADIUS);
				
				if (menu.preview_exists) { // has save, has preview
					// lotta memory churn here
					SDL_Surface* bmp = IMG_Load(menu.bmp_path);
					SDL_Surface* raw_preview = SDL_ConvertSurfaceFormat(bmp, screen->format->format,0);
					if (raw_preview) {
						SDL_FreeSurface(bmp); 
						bmp = raw_preview; 
					}
					// LOG_info("raw_preview %ix%i\n", raw_preview->w,raw_preview->h);
					SDL_Rect preview_rect = {ox,oy,hw,hh};
					SDL_FillRect(screen, &preview_rect, SDL_MapRGBA(screen->format,0,0,0,255));
					SDL_BlitScaled(bmp,NULL,preview,NULL);
					SDL_BlitSurface(preview, NULL, screen, &(SDL_Rect){ox,oy});
					SDL_FreeSurface(bmp);
				}
				else {
					SDL_Rect preview_rect = {ox,oy,hw,hh};
					SDL_FillRect(screen, &preview_rect, SDL_MapRGBA(screen->format,0,0,0,255));
					if (menu.save_exists) GFX_blitMessage(font.large, T("menu.no_preview"), screen, &preview_rect);
					else GFX_blitMessage(font.large, T("menu.empty_slot"), screen, &preview_rect);
				}
				
				// pagination
				ox += (pw-SCALE1(15*MENU_SLOT_COUNT))/2;
				oy += hh+SCALE1(WINDOW_RADIUS);
				for (int i=0; i<MENU_SLOT_COUNT; i++) {
					if (i==menu.slot)GFX_blitAsset(ASSET_PAGE, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15),oy});
					else GFX_blitAsset(ASSET_DOT, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15)+4,oy+SCALE1(2)});
				}
			}
			GFX_flip(screen);
			dirty=0;
		} else {
			// please dont flip cause it will cause current_fps dip and audio is weird first seconds
			GFX_delay();
		}
		hdmimon();
	}
	
	SDL_FreeSurface(preview);
	if(menu.bitmap) SDL_FreeSurface(menu.bitmap);
	PAD_reset();

	GFX_clearAll();
	
	int count = 0;
	char** overlayList = config.frontend.options[FE_OPT_OVERLAY].values;
	while ( overlayList && overlayList[count]) count++;
	if (overlay >= 0 && overlay < count) {
		GFX_setOverlay(overlayList[overlay],core.tag);
	}

	GFX_setOffsetX(cfg_screenx);
	GFX_setOffsetY(cfg_screeny);
	if (!quit) {
		if (restore_w!=DEVICE_WIDTH || restore_h!=DEVICE_HEIGHT) {
			screen = GFX_resize(restore_w,restore_h,restore_p);
		}
		GFX_setEffect(screen_effect);

		GFX_clear(screen);

		setOverclock(overclock); // restore overclock value
		if (rumble_strength) VIB_setStrength(rumble_strength);
		
		if (!HAS_POWER_BUTTON) PWR_disableSleep();
	}
	else if (exists(NOUI_PATH)) PWR_powerOff(0); // TODO: won't work with threaded core, only check this once per launch
	

	SDL_FreeSurface(backing);
	PWR_disableAutosleep();
}

void Menu_setCoreVersionDesc(const char* version) {
	options_menu.items[1].desc = (char*)version;
}

void Menu_waitScreenshot(void) {
	SDL_WaitThread(screenshotsavethread, NULL);
}

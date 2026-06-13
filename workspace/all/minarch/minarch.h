/*
 * NextUI Minarch Header
 * Shared types and accessor/API functions for minarch menu system
 * Use these to avoid symbol conflicts with cores and struct layout mismatches with LTO
 */

#ifndef MINARCH_H
#define MINARCH_H

#include <SDL2/SDL.h>
#include <stddef.h>

// Menu callback result codes
// Guarded so it can coexist with the identical enum in ma_frontend_opts.h
// (minarch.c and ma_menu.c pull in both headers).
#ifndef MENU_CALLBACK_CODES_DEFINED
#define MENU_CALLBACK_CODES_DEFINED
enum {
    MENU_CALLBACK_NOP = 0,
    MENU_CALLBACK_EXIT = 1,
    MENU_CALLBACK_NEXT_ITEM = 2,
};
#endif

// Screen/display accessors
SDL_Surface* minarch_getScreen(void);
int minarch_getDeviceWidth(void);
int minarch_getDeviceHeight(void);
SDL_Surface* minarch_getMenuBitmap(void);

// Game state accessors
const char* minarch_getCoreTag(void);
const char* minarch_getGameName(void);
void* minarch_getGameData(void);
size_t minarch_getGameSize(void);

// Core option accessors
char* minarch_getCoreOptionValue(const char* key);
void minarch_setCoreOptionValue(const char* key, const char* value);

// Batch mode for setting multiple core options atomically
void minarch_beginOptionsBatch(void);
void minarch_endOptionsBatch(void);

// Force core to process option changes immediately
// Runs one frame with video output suppressed to trigger check_variables()
void minarch_forceCoreOptionUpdate(void);

// Save current config to file
void minarch_saveConfig(void);

// Reload game to apply option changes (e.g., gpSP serial mode)
// Unloads and reloads ROM so core re-reads options during load_game()
void minarch_reloadGame(void);

// Sleep state accessors
void minarch_beforeSleep(void);
void minarch_afterSleep(void);

// Platform accessors
void minarch_hdmimon(void);

// Menu accessors
int minarch_menuMessage(char* message, char** pairs);

#endif /* MINARCH_H */

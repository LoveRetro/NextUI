#pragma once

#include "ma_internal.h"

// -----------------------------------------------------------------------
// Menu type system
// -----------------------------------------------------------------------

typedef struct MenuList MenuList;
typedef struct MenuItem MenuItem;

#ifndef MENU_CALLBACK_CODES_DEFINED
#define MENU_CALLBACK_CODES_DEFINED
enum {
	MENU_CALLBACK_NOP,
	MENU_CALLBACK_EXIT,
	MENU_CALLBACK_NEXT_ITEM,
};
#endif

typedef int (*MenuList_callback_t)(MenuList* list, int i);

typedef struct MenuItem {
	char* name;
	char* desc;
	char** values;
	char* key; // optional, used by options
	int id; // optional, used by bindings
	int value;
	MenuList* submenu;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
} MenuItem;

enum {
	MENU_LIST, // eg. save and main menu
	MENU_VAR, // eg. frontend
	MENU_FIXED, // eg. emulator
	MENU_INPUT, // eg. renders like but MENU_VAR but handles input differently
};

typedef struct MenuList {
	int type;
	int max_width; // cached on first draw
	char* desc;
	char* category; // currently displayed category
	MenuItem* items;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
} MenuList;

// -----------------------------------------------------------------------
// Functions defined in ma_menu.c (see ma_menu.h)
// -----------------------------------------------------------------------
#include "ma_menu.h"

// -----------------------------------------------------------------------
// Public API: functions defined in ma_frontend_opts.c
// -----------------------------------------------------------------------

// Menu utility (also called from minarch.c achievement section)
int Menu_message(char* message, char** pairs);
int Menu_messageWithFont(char* message, char** pairs, TTF_Font* f);

// Helper (also called from minarch.c)
char* getSaveDesc(void);

// Option menus (called from options_menu in minarch.c)
int OptionFrontend_openMenu(MenuList* list, int i);
int OptionEmulator_openMenu(MenuList* list, int index);
int OptionShaders_openMenu(MenuList* list, int i);
int OptionCheats_openMenu(MenuList* list, int i);
int OptionControls_openMenu(MenuList* list, int i);
int OptionShortcuts_openMenu(MenuList* list, int i);
int OptionSaveChanges_openMenu(MenuList* list, int i);

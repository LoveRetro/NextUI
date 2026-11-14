#pragma once

typedef struct MenuList MenuList; // forward declaration (full def in minarch_frontend_opts.h)

void Menu_init(void);
void Menu_quit(void);
void Menu_beforeSleep(void);
void Menu_afterSleep(void);
int  Menu_options(MenuList* list);
void Menu_screenshot(void);
void Menu_saveState(void);
void Menu_loadState(void);
void OptionSaveChanges_updateDesc(void);
void OptionAchievements_updateDesc(void);
bool getAlias(char* path, char* alias);
int  save_screenshot_thread(void* data);
void MSG_init(void);
void MSG_quit(void);
void Menu_initState(void);
void Menu_loop(void);
void Menu_setCoreVersionDesc(const char* version);
void Menu_waitScreenshot(void);

#pragma once

extern int state_slot;

// User-facing state slots 1-8 are internal 0-7; 8 is the hidden default state
// and AUTO_RESUME_SLOT (9) the auto save.
#define USER_STATE_SLOTS 8
// The user slot this session resumed from, or last saved/loaded successfully,
// -1 if none yet. Target of the in-game menu's Save & Quit.
extern int session_slot;

void SRAM_read(void);
void SRAM_write(void);
void RTC_read(void);
void RTC_write(void);
void State_getPath(char* filename);
int  State_read(void);
int  State_write(void);
void State_autosave(void);
void State_resume(void);

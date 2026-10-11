#pragma once

extern int state_slot;

void SRAM_read(void);
void SRAM_write(void);
void RTC_read(void);
void RTC_write(void);
void State_getPath(char* filename);
int  State_read(void);
int  State_write(void);
void State_autosave(void);
void State_resume(void);

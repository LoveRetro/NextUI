#pragma once

#include "libretro.h"
#include <stdint.h>

int setFastForward(int enable);
void input_poll_callback(void);
int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id);
void Input_init(const struct retro_input_descriptor *vars);

// Menu functions defined in minarch.c, called from minarch_input.c
void Menu_beforeSleep(void);
void Menu_afterSleep(void);
void Menu_screenshot(void);
void Menu_saveState(void);
void Menu_loadState(void);

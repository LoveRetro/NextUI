#pragma once

#include "libretro.h"

int setFastForward(int enable);
void input_poll_callback(void);
int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id);
void Input_init(const struct retro_input_descriptor *vars);

// Current local RETRO_DEVICE_ID_JOYPAD_* button bitmask (for netplay input sync).
uint32_t Input_getButtons(void);

// Menu functions (defined in ma_menu.c, see ma_menu.h)
#include "ma_menu.h"

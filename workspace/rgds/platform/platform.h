// rgds: Anbernic RG DS (RK3568) on the ROCKNIX kernel, SDL2 KMSDRM, NDS only

#ifndef PLATFORM_H
#define PLATFORM_H

///////////////////////////////

#ifdef SDL
#	include "sdl.h"
#endif

///////////////////////////////

#define BUTTON_UP		BUTTON_NA
#define BUTTON_DOWN		BUTTON_NA
#define BUTTON_LEFT		BUTTON_NA
#define BUTTON_RIGHT	BUTTON_NA

#define BUTTON_SELECT	BUTTON_NA
#define BUTTON_START	BUTTON_NA

#define BUTTON_A		BUTTON_NA
#define BUTTON_B		BUTTON_NA
#define BUTTON_X		BUTTON_NA
#define BUTTON_Y		BUTTON_NA

#define BUTTON_L1		BUTTON_NA
#define BUTTON_R1		BUTTON_NA
#define BUTTON_L2		BUTTON_NA
#define BUTTON_R2		BUTTON_NA
#define BUTTON_L3		BUTTON_NA
#define BUTTON_R3		BUTTON_NA
#define BUTTON_L4		BUTTON_NA
#define BUTTON_R4		BUTTON_NA

#define BUTTON_MENU		BUTTON_NA
#define BUTTON_MENU_ALT	BUTTON_NA
#define	BUTTON_POWER	BUTTON_NA // power key is owned by powerd
#define	BUTTON_PLUS		BUTTON_NA
#define	BUTTON_MINUS	BUTTON_NA

///////////////////////////////

#define CODE_UP			CODE_NA
#define CODE_DOWN		CODE_NA
#define CODE_LEFT		CODE_NA
#define CODE_RIGHT		CODE_NA

#define CODE_SELECT		CODE_NA
#define CODE_START		CODE_NA

#define CODE_A			CODE_NA
#define CODE_B			CODE_NA
#define CODE_X			CODE_NA
#define CODE_Y			CODE_NA

#define CODE_L1			CODE_NA
#define CODE_R1			CODE_NA
#define CODE_L2			CODE_NA
#define CODE_R2			CODE_NA
#define CODE_L3			CODE_NA
#define CODE_R3			CODE_NA
#define CODE_L4         CODE_NA
#define CODE_R4         CODE_NA

#define CODE_MENU		CODE_NA
#define CODE_MENU_ALT	CODE_NA
#define CODE_POWER		CODE_NA

#define CODE_PLUS		CODE_NA
#define CODE_MINUS		CODE_NA

///////////////////////////////
						// HATS
// SDL raw joystick indices for gpio-keys-control: buttons are numbered in evdev code order,
// BTN_SOUTH(0x130)..BTN_THUMBR(0x13e) = 0..12, then BTN_DPAD_UP..RIGHT(0x220..0x223) = 13..16.
#define JOY_UP			13
#define JOY_DOWN		14
#define JOY_LEFT		15
#define JOY_RIGHT		16

#define JOY_SELECT		8
#define JOY_START		9

// TODO: these ended up swapped in the first public release of stock :sob:
#define JOY_A			1
#define JOY_B			0
#define JOY_X			2
#define JOY_Y			3

#define JOY_L1			4
#define JOY_R1			5
#define JOY_L2			6
#define JOY_R2			7
#define JOY_L3			11
#define JOY_R3			12
#define JOY_L4          JOY_NA
#define JOY_R4          JOY_NA

#define JOY_MENU		10
#define JOY_POWER		JOY_NA
#define JOY_PLUS		JOY_NA
#define JOY_MINUS		JOY_NA

///////////////////////////////
// USER-ASSIGNABLE BUTTONS
#define BTN_FN1			BTN_NONE
#define BTN_FN2			BTN_NONE
#define BTN_FN3			BTN_NONE
#define BTN_FN1_NAME	""
#define BTN_FN2_NAME	""
#define BTN_FN3_NAME	""

///////////////////////////////

#define AXIS_L2			AXIS_NA // digital triggers
#define AXIS_R2			AXIS_NA

#define AXIS_LX			0 // ABS_X, -30k (left) to 30k (right)
#define AXIS_LY			1 // ABS_Y, -30k (up) to 30k (down)
#define AXIS_RX			3 // ABS_RX, -30k (left) to 30k (right)
#define AXIS_RY			4 // ABS_RY, -30k (up) to 30k (down)

///////////////////////////////

#define BTN_RESUME			BTN_X
#define BTN_SLEEP 			BTN_NONE
#define BTN_WAKE 			BTN_NONE
#define BTN_MOD_VOLUME 		BTN_NONE
#define BTN_MOD_BRIGHTNESS 	BTN_MENU
#define BTN_MOD_COLORTEMP 	BTN_SELECT
#define BTN_MOD_PLUS 		BTN_R1
#define BTN_MOD_MINUS 		BTN_L1

///////////////////////////////

#define FIXED_SCALE 	2
#define FIXED_WIDTH		640
#define FIXED_HEIGHT	480
#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)
#define FIXED_SIZE		(FIXED_PITCH * FIXED_HEIGHT)

///////////////////////////////

#define MAIN_ROW_COUNT  6
#define QUICK_SWITCHER_COUNT 4
#define PADDING 10

///////////////////////////////

#define SDCARD_PATH "/mnt/SDCARD"
#define MUTE_VOLUME_RAW 0

// panel_description: clock=42134 kHz, 640+260+220+260 x 480+10+2+16
#define SCREEN_FPS 59.98

#define MAX_LIGHTS 0

///////////////////////////////

#endif

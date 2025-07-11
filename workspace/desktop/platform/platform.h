// tg5040

#ifndef PLATFORM_H
#define PLATFORM_H

///////////////////////////////

#include "sdl.h"

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

#define BUTTON_MENU		BUTTON_NA
#define BUTTON_MENU_ALT	BUTTON_NA
#define	BUTTON_POWER	BUTTON_NA
#define	BUTTON_PLUS		BUTTON_NA
#define	BUTTON_MINUS	BUTTON_NA

///////////////////////////////

// see https://wiki.libsdl.org/SDL2/SDL_Scancode

#define CODE_UP			82 // Up Arrow
#define CODE_DOWN		81 // Down Arrow
#define CODE_LEFT		80 // Left Arrow
#define CODE_RIGHT		79 // Right Arrow

#define CODE_SELECT		53 // ^
#define CODE_START		40 // Return

#define CODE_A			22 // S
#define CODE_B			4  // A
#define CODE_X			26 // W
#define CODE_Y			20 // Q

#define CODE_L1			CODE_NA
#define CODE_R1			CODE_NA
#define CODE_L2			CODE_NA
#define CODE_R2			CODE_NA
#define CODE_L3			CODE_NA
#define CODE_R3			CODE_NA

#define CODE_MENU		44 // Space
#define CODE_POWER		42 // Backspace

#define CODE_PLUS		CODE_NA
#define CODE_MINUS		CODE_NA

///////////////////////////////
						// HATS
#define JOY_UP			JOY_NA
#define JOY_DOWN		JOY_NA
#define JOY_LEFT		JOY_NA
#define JOY_RIGHT		JOY_NA

#define JOY_SELECT		JOY_NA
#define JOY_START		JOY_NA

// TODO: these ended up swapped in the first public release of stock :sob:
#define JOY_A			JOY_NA
#define JOY_B			JOY_NA
#define JOY_X			JOY_NA
#define JOY_Y			JOY_NA

#define JOY_L1			JOY_NA
#define JOY_R1			JOY_NA
#define JOY_L2			JOY_NA
#define JOY_R2			JOY_NA
#define JOY_L3			JOY_NA
#define JOY_R3			JOY_NA

#define JOY_MENU		JOY_NA
#define JOY_POWER		JOY_NA
#define JOY_PLUS		JOY_NA
#define JOY_MINUS		JOY_NA

///////////////////////////////

#define BTN_RESUME			BTN_X
#define BTN_SLEEP 			BTN_POWER
#define BTN_WAKE 			BTN_POWER
#define BTN_MOD_VOLUME 		BTN_NONE
#define BTN_MOD_COLORTEMP	BTN_NONE
#define BTN_MOD_BRIGHTNESS 	BTN_MENU
#define BTN_MOD_PLUS 		BTN_PLUS
#define BTN_MOD_MINUS 		BTN_MINUS

///////////////////////////////

// average smallish screen
//#define FIXED_SCALE 	2
//#define FIXED_WIDTH		640
//#define FIXED_HEIGHT	480
//#define MAIN_ROW_COUNT 6
//#define PADDING 10

// emulate Brick
#define FIXED_SCALE 	3
#define FIXED_WIDTH		1024
#define FIXED_HEIGHT	768
#define MAIN_ROW_COUNT 7
#define QUICK_SWITCHER_COUNT 3
#define PADDING 5

// emulate TSP
//#define FIXED_SCALE 	2
//#define FIXED_WIDTH		1280
//#define FIXED_HEIGHT	720
//#define MAIN_ROW_COUNT  10
//#define QUICK_SWITCHER_COUNT 4
//#define PADDING 10

#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)
#define FIXED_SIZE		(FIXED_PITCH * FIXED_HEIGHT)

// #define HAS_HDMI	1
// #define HDMI_WIDTH 	1280
// #define HDMI_HEIGHT 720
// #define HDMI_PITCH 	(HDMI_WIDTH * FIXED_BPP)
// #define HDMI_SIZE	(HDMI_PITCH * HDMI_HEIGHT)

///////////////////////////////



///////////////////////////////

#define SDCARD_PATH "/Library/Developer/Projects/private/MinUI_FAKESD"
#define MUTE_VOLUME_RAW 63 // 0 unintuitively is 100% volume

#define MAX_LIGHTS 4

// this should be set to the devices native screen refresh rate
#define SCREEN_FPS 60.0
///////////////////////////////

#endif

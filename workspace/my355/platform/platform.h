// my355/platform/platform.h

#ifndef PLATFORM_H
#define PLATFORM_H

///////////////////////////////

#ifdef SDL
#	include "sdl.h"
#endif

///////////////////////////////
#define	BUTTON_UP		BUTTON_NA
#define	BUTTON_DOWN		BUTTON_NA
#define	BUTTON_LEFT		BUTTON_NA
#define	BUTTON_RIGHT	BUTTON_NA

#define	BUTTON_SELECT	BUTTON_NA
#define	BUTTON_START	BUTTON_NA

#define	BUTTON_A		BUTTON_NA
#define	BUTTON_B		BUTTON_NA
#define	BUTTON_X		BUTTON_NA
#define	BUTTON_Y		BUTTON_NA

#define	BUTTON_L1		BUTTON_NA
#define	BUTTON_R1		BUTTON_NA
#define	BUTTON_L2		BUTTON_NA
#define	BUTTON_R2		BUTTON_NA
#define BUTTON_L3 		BUTTON_NA
#define BUTTON_R3 		BUTTON_NA
#define BUTTON_L4		BUTTON_NA
#define BUTTON_R4		BUTTON_NA

#define	BUTTON_MENU		BUTTON_NA
#define	BUTTON_POWER	BUTTON_NA
#define	BUTTON_PLUS		BUTTON_NA
#define	BUTTON_MINUS	BUTTON_NA

///////////////////////////////

// Every button is reported twice: by the gpio-keys-polled kernel driver and by
// the gamepad miyoo_inputd creates. Only the gamepad is used.
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
#define CODE_POWER		102

#define CODE_PLUS		128
#define CODE_MINUS		129

///////////////////////////////

						// HATS
#define JOY_UP			JOY_NA
#define JOY_DOWN		JOY_NA
#define JOY_LEFT		JOY_NA
#define JOY_RIGHT		JOY_NA

#define JOY_SELECT		6
#define JOY_START		7

#define JOY_A			1
#define JOY_B			0
#define JOY_X			3
#define JOY_Y			2

#define JOY_L1			4
#define JOY_R1			5
#define JOY_L2			JOY_NA
#define JOY_R2			JOY_NA
#define JOY_L3			9
#define JOY_R3			10
#define JOY_L4			JOY_NA
#define JOY_R4			JOY_NA

#define JOY_MENU		8
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

#define AXIS_L2	2
#define AXIS_R2	5

#define AXIS_LX	0
#define AXIS_LY	1
#define AXIS_RX	3
#define AXIS_RY	4

///////////////////////////////

#define BTN_RESUME			BTN_X
#define BTN_SLEEP 			BTN_POWER
#define BTN_WAKE 			BTN_POWER
#define BTN_MOD_VOLUME 		BTN_NONE
#define BTN_MOD_BRIGHTNESS 	BTN_MENU
#define BTN_MOD_PLUS 		BTN_PLUS
#define BTN_MOD_MINUS 		BTN_MINUS
#define BTN_MOD_COLORTEMP	BTN_NONE

///////////////////////////////

int PLAT_onHDMI(void);


#define HAS_HDMI	1
#define HDMI_WIDTH 	1280
#define HDMI_HEIGHT 720
#define HDMI_PITCH 	(HDMI_WIDTH * FIXED_BPP)
#define HDMI_SIZE	(HDMI_PITCH * HDMI_HEIGHT)

///////////////////////////////

#define FIXED_SCALE 	2
#define FIXED_WIDTH		(PLAT_onHDMI()?HDMI_WIDTH:640)
#define FIXED_HEIGHT	(PLAT_onHDMI()?HDMI_HEIGHT:480)
#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)
#define FIXED_SIZE		(FIXED_PITCH * FIXED_HEIGHT)

///////////////////////////////

// 720p values match tg5050
#define MAIN_ROW_COUNT (PLAT_onHDMI()?10:6)
#define QUICK_SWITCHER_COUNT (PLAT_onHDMI()?4:3)
#define PADDING 10

///////////////////////////////

#define SDCARD_PATH "/mnt/SDCARD"
#define MUTE_VOLUME_RAW 0
// #define HAS_NEON

// this should be set to the devices native screen refresh rate
// modetest -M rockchip -c: 640x480, clk 32000kHz, htotal 1030, vtotal 518
// 32000000 / (1030 * 518) = 59.9768
#define SCREEN_FPS 59.9768
///////////////////////////////

#endif

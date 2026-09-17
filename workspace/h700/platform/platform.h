// h700

#ifndef PLATFORM_H
#define PLATFORM_H

///////////////////////////////

#ifdef SDL
#	include "sdl.h"
#endif

///////////////////////////////

extern int panel_w;
extern int panel_h;
extern double panel_fps;
extern int dev_has_lstick;
extern int dev_has_rstick;
extern int dev_has_rgb;
extern int dev_num_leds;
extern int needs_portrait_sdl; // DEVICE=rg28xx: SDL rotates onto the portrait panel

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
#define	BUTTON_POWER	116
#define	BUTTON_PLUS		BUTTON_NA
#define	BUTTON_MINUS	BUTTON_NA

///////////////////////////////

#define CODE_UP			103
#define CODE_DOWN		108
#define CODE_LEFT		105
#define CODE_RIGHT		106

#define CODE_SELECT		310
#define CODE_START		311

#define CODE_A			304
#define CODE_B			305
#define CODE_X			307
#define CODE_Y			306

#define CODE_L1			308
#define CODE_R1			309
#define CODE_L2			314
#define CODE_R2			315
#define CODE_L4			CODE_NA
#define CODE_R4			CODE_NA
#define CODE_L3			(dev_has_lstick ? 313 : CODE_NA)
#define CODE_R3			(dev_has_rstick ? 316 : CODE_NA)

#define CODE_MENU		312
#define CODE_MENU_ALT	354
#define CODE_POWER		116

#define CODE_PLUS		115
#define CODE_MINUS		114

///////////////////////////////
						// HATS
#define JOY_UP			JOY_NA
#define JOY_DOWN		JOY_NA
#define JOY_LEFT		JOY_NA
#define JOY_RIGHT		JOY_NA

#define JOY_SELECT		6
#define JOY_START		7

#define JOY_A			0
#define JOY_B			1
#define JOY_X			3
#define JOY_Y			2

#define JOY_L1			4
#define JOY_R1			5
#define JOY_L2			10
#define JOY_R2			11
#define JOY_L4			JOY_NA
#define JOY_R4			JOY_NA
#define JOY_L3			(dev_has_lstick ? 9 : JOY_NA)
#define JOY_R3			(dev_has_rstick ? 12 : JOY_NA)

#define JOY_MENU		8
#define JOY_POWER		JOY_NA
#define JOY_PLUS		16
#define JOY_MINUS		15

///////////////////////////////
// USER-ASSIGNABLE BUTTONS
// H700 devices have no dedicated FN1/FN2/HOME buttons for pak launch actions.
#define BTN_FN1			BTN_NONE
#define BTN_FN2			BTN_NONE
#define BTN_FN3			BTN_NONE
#define BTN_FN1_NAME	""
#define BTN_FN2_NAME	""
#define BTN_FN3_NAME	""

///////////////////////////////

#define AXIS_L2			AXIS_NA
#define AXIS_R2			AXIS_NA

#define AXIS_LX			(dev_has_lstick ? 0 : AXIS_NA)
#define AXIS_LY			(dev_has_lstick ? 1 : AXIS_NA)
#define AXIS_RX			(dev_has_rstick ? 2 : AXIS_NA)
#define AXIS_RY			(dev_has_rstick ? 3 : AXIS_NA)

///////////////////////////////

#define BTN_RESUME			BTN_X
#define BTN_SLEEP 			BTN_POWER
#define BTN_WAKE 			BTN_POWER
#define BTN_MOD_VOLUME 		BTN_NONE
#define BTN_MOD_BRIGHTNESS 	BTN_MENU
#define BTN_MOD_COLORTEMP 	BTN_SELECT
#define BTN_MOD_PLUS 		BTN_PLUS
#define BTN_MOD_MINUS 		BTN_MINUS

///////////////////////////////

// While an HDMI cable is connected the whole app runs at 1280x720 (the fb is
// hardware-scaled to a 1080p60 signal by the display engine — see SetHDMI in
// libmsettings). GFX_init latches hdmi_active, and PLAT_initPlatform detects
// panel_w/panel_h. The existing hotplug quit-and-relaunch plumbing restarts
// apps on cable changes.
// Values must match HDMI_LOGICAL_* in libmsettings/msettings.c.
#define HAS_HDMI		1
#define HDMI_WIDTH		1280
#define HDMI_HEIGHT		720
#define HDMI_PITCH		(HDMI_WIDTH * FIXED_BPP)
#define HDMI_SIZE		(HDMI_PITCH * HDMI_HEIGHT)

#define FIXED_SCALE 	2
// panel_w/panel_h: app framebuffer — 720x480 (rg34xx/rg34xxsp/rgsp),
// 720x720 (rgcubexx), else 640x480. RG28XX stays 640x480 here; portrait is
// handled by SDL_ROTATION via needs_portrait_sdl.
#define FIXED_WIDTH		(hdmi_active?HDMI_WIDTH:panel_w)
#define FIXED_HEIGHT	(hdmi_active?HDMI_HEIGHT:panel_h)
#define FIXED_BPP		2
#define FIXED_DEPTH		(FIXED_BPP * 8)
#define FIXED_PITCH		(FIXED_WIDTH * FIXED_BPP)
#define FIXED_SIZE		(FIXED_PITCH * FIXED_HEIGHT)

///////////////////////////////

// Rows that fit above the button hints: (FIXED_HEIGHT/FIXED_SCALE - 2*PADDING - PILL_SIZE) / PILL_SIZE.
// The 480p layout needs the standard 10-unit padding so six rows and the
// bottom hints keep the same vertical spacing. The roomier 720p layouts retain
// their existing 5-unit edge padding.
#define MAIN_ROW_COUNT ((hdmi_active||panel_h>=720)?10:6)
#define QUICK_SWITCHER_COUNT 3
#define PADDING ((hdmi_active||panel_h>=720)?5:10)

///////////////////////////////

#define SDCARD_PATH "/mnt/SDCARD"
#define MUTE_VOLUME_RAW 0

// Stock H700 audio must be closed before suspend to avoid a long stall on wake.
#define SND_CLOSE_ON_SLEEP 1

#define SCREEN_FPS (hdmi_active ? 60.0 : panel_fps)
// ceiling, not the count: only rg40xxh/v and rgcubexx have RGB LEDs, and
// the V populates one bank where the others populate two. PLAT_getNumLeds()
// reports what the running device actually has.
#define MAX_LIGHTS 2

///////////////////////////////

#endif

#ifndef __MANUAL_HOST_H__
#define __MANUAL_HOST_H__

#include <SDL2/SDL.h>

typedef enum ManualButton {
	MANUAL_BTN_A = 0,
	MANUAL_BTN_B,
	MANUAL_BTN_UP,
	MANUAL_BTN_DOWN,
	MANUAL_BTN_LEFT,
	MANUAL_BTN_RIGHT,
	MANUAL_BTN_L1,
	MANUAL_BTN_R1,
} ManualButton;

typedef struct ManualHost {
	SDL_Surface** screen;
	int* quit;
	void (*before_sleep)(void);
	void (*after_sleep)(void);
	void (*start_frame)(void);
	void (*poll_input)(void);
	void (*reset_input)(void);
	int (*just_pressed)(ManualButton button);
	int (*is_pressed)(ManualButton button);
	void (*power_update)(int* dirty);
	void (*clear)(SDL_Surface* screen);
	void (*flip)(SDL_Surface* screen);
	void (*delay)(void);
	void (*draw_notice)(SDL_Surface* screen, const char* msg);
	void (*draw_overlay)(SDL_Surface* screen, const char* page_info);
} ManualHost;

#endif

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <msettings.h>

#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "utils.h"

// full-scale SDL joystick axis magnitude, used to map raw axis values to on-screen nub travel
#define STICK_AXIS_MAX 32767

static int getButtonWidth(char* label) {
	SDL_Surface* text;
	int w = 0;
	
	if (strlen(label)<=2) w = SCALE1(BUTTON_SIZE);
	else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, ALT_BUTTON_TEXT_COLOR);
		w = SCALE1(BUTTON_SIZE) + text->w;
		SDL_FreeSurface(text);
	}
	return w;
}

static void blitButton(char* label, SDL_Surface* dst, int pressed, int x, int y, int w) {
	SDL_Rect point = {x,y};
	SDL_Surface* text;

	// Pressed reads as a lit button (main fill + accent2 label, matching GFX_blitButton),
	// released reads as a hole punched through the tray to the background.
	uint32_t fill = pressed ? THEME_COLOR1 : THEME_COLOR7;
	SDL_Color text_color = pressed ? ALT_BUTTON_TEXT_COLOR : uintToColour(THEME_COLOR6_255);

	int len = strlen(label);
	if (len<=2) {
		text = TTF_RenderUTF8_Blended(len==2?font.small:font.medium, label, text_color);
		GFX_blitAssetColor(ASSET_BUTTON, NULL, dst, &point, fill);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x+(SCALE1(BUTTON_SIZE)-text->w)/2,point.y+(SCALE1(BUTTON_SIZE)-text->h)/2});
	}
	else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, text_color);
		w = w ? w : SCALE1(BUTTON_SIZE)/2+text->w;
		GFX_blitPillColor(ASSET_BUTTON, dst, &(SDL_Rect){point.x,point.y,w,SCALE1(BUTTON_SIZE)}, fill, RGB_WHITE);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x+(w-text->w)/2,point.y+(SCALE1(BUTTON_SIZE)-text->h)/2,text->w,text->h});
	}

	SDL_FreeSurface(text);
}

// ASSET_WHITE_PILL's round caps are a fixed-size sprite (native PILL_SIZE), so it can't be
// stretched into a bigger circle without sampling outside the sprite. Draw one directly instead.
static void fillCircle(SDL_Surface* dst, int cx, int cy, int radius, uint32_t color) {
	for (int dy = -radius; dy <= radius; dy++) {
		int dx = (int)(sqrt((double)radius*radius - (double)dy*dy) + 0.5);
		SDL_FillRect(dst, &(SDL_Rect){cx-dx, cy+dy, dx*2, 1}, color);
	}
}

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_AUTO);
	
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();
	
	// one-time
	int has_L2 = (BUTTON_L2!=BUTTON_NA || CODE_L2!=CODE_NA || JOY_L2!=JOY_NA || AXIS_L2!=AXIS_NA);
	int has_R2 = (BUTTON_R2!=BUTTON_NA || CODE_R2!=CODE_NA || JOY_R2!=JOY_NA || AXIS_R2!=AXIS_NA);
	int has_L3 = (BUTTON_L3!=BUTTON_NA || CODE_L3!=CODE_NA || JOY_L3!=JOY_NA);
	int has_R3 = (BUTTON_R3!=BUTTON_NA || CODE_R3!=CODE_NA || JOY_R3!=JOY_NA);
	int has_L4 = (BUTTON_L4!=BUTTON_NA || CODE_L4!=CODE_NA || JOY_L4!=JOY_NA);
	int has_R4 = (BUTTON_R4!=BUTTON_NA || CODE_R4!=CODE_NA || JOY_R4!=JOY_NA);

	int has_volume = (BUTTON_PLUS!=BUTTON_NA || CODE_PLUS!=CODE_NA || JOY_PLUS!=JOY_NA);
	int has_power = HAS_POWER_BUTTON;
	int has_menu = HAS_MENU_BUTTON;
	int has_both = (has_power && has_menu);
	
	int oy = SCALE1(PADDING);
	if (!has_L3 && !has_R3) oy += SCALE1(PILL_SIZE);
	
	SDL_Event event;
	int quit = 0;
	int dirty = 1;
	// int show_setting = 0;
	PAD_Axis last_laxis = {0,0};
	PAD_Axis last_raxis = {0,0};
	while(!quit) {
		GFX_startFrame();
		uint32_t frame_start = SDL_GetTicks();

		PAD_poll();

		if (PAD_anyPressed() || PAD_anyJustReleased()) dirty = 1;
		if (pad.laxis.x!=last_laxis.x || pad.laxis.y!=last_laxis.y || pad.raxis.x!=last_raxis.x || pad.raxis.y!=last_raxis.y) {
			dirty = 1;
			last_laxis = pad.laxis;
			last_raxis = pad.raxis;
		}
		if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_START)) quit = 1;
		
		// PWR_update(&dirty, NULL, NULL,NULL);
		
		if (dirty) {
			GFX_clear(screen);
			
			// GFX_blitHardwareGroup(screen, show_setting);
			
			// L group
			{
				int x = SCALE1(BUTTON_MARGIN + PADDING);
				int y = oy;
				int w = 0;
				int ox = 0;
			
				w = getButtonWidth("L1") + SCALE1(BUTTON_MARGIN)*2;
				ox = w;
			
				if (has_L2) w += getButtonWidth("L2") + SCALE1(BUTTON_MARGIN);
				if (has_L4) w += getButtonWidth("L4") + SCALE1(BUTTON_MARGIN);
				if (!has_L2 && !has_L4) x += SCALE1(PILL_SIZE);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w});

				blitButton("L1", screen, PAD_isPressed(BTN_L1), x + SCALE1(BUTTON_MARGIN), y+SCALE1(BUTTON_MARGIN),0);
				if (has_L2) {
					x += ox;
					blitButton("L2", screen, PAD_isPressed(BTN_L2), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				if (has_L4) {
					x += ox - SCALE1(BUTTON_MARGIN);
					blitButton("L4", screen, PAD_isPressed(BTN_L4), x, y+SCALE1(BUTTON_MARGIN),0);
				} 
			}
			
			// R group
			{
				int x = 0;
				int y = oy;
				int w = 0;
				int ox = 0;
			
				w = getButtonWidth("R1") + SCALE1(BUTTON_MARGIN)*2;
				ox = w;
			
				if (has_R2) w += getButtonWidth("R2") + SCALE1(BUTTON_MARGIN);
				if (has_R4) w += getButtonWidth("R4") + SCALE1(BUTTON_MARGIN);
				
				x = screen->w - w - SCALE1(BUTTON_MARGIN + PADDING);
				if (!has_R2 && !has_R4) x -= SCALE1(PILL_SIZE);
					
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,w});

				if(has_R4) {
					x+= SCALE1(BUTTON_MARGIN);
					blitButton("R4", screen, PAD_isPressed(BTN_R4), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				if(has_R2) {
					x += has_R4 ? ox - SCALE1(BUTTON_MARGIN) : SCALE1(BUTTON_MARGIN);
					blitButton("R2", screen, PAD_isPressed(BTN_R2), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				x += (has_R4 || has_R2) ? ox - SCALE1(BUTTON_MARGIN) : SCALE1(BUTTON_MARGIN);
				blitButton("R1", screen, PAD_isPressed(BTN_R1), x, y+SCALE1(BUTTON_MARGIN),0);
			}
			
			// DPAD group
			{
				// UP
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*2);
				int o = SCALE1(BUTTON_MARGIN);
				
				SDL_FillRect(screen, &(SDL_Rect){x,y+SCALE1(PILL_SIZE/2),SCALE1(PILL_SIZE),SCALE1(PILL_SIZE*2)}, THEME_COLOR2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("U", screen, PAD_isPressed(BTN_DPAD_UP), x+o, y+o,0);
				
				y += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("D", screen, PAD_isPressed(BTN_DPAD_DOWN), x+o, y+o,0);
				
				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);
				
				SDL_FillRect(screen, &(SDL_Rect){x+SCALE1(PILL_SIZE/2),y,SCALE1(PILL_SIZE*2),SCALE1(PILL_SIZE)}, THEME_COLOR2);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("L", screen, PAD_isPressed(BTN_DPAD_LEFT), x+o, y+o,0);
				
				x += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("R", screen, PAD_isPressed(BTN_DPAD_RIGHT), x+o, y+o,0);
			}
			
			// ABXY group
			{
				// UP
				int x = screen->w - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*2);
				int o = SCALE1(BUTTON_MARGIN);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("X", screen, PAD_isPressed(BTN_X), x+o, y+o,0);
				
				y += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("B", screen, PAD_isPressed(BTN_B), x+o, y+o,0);
				
				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("Y", screen, PAD_isPressed(BTN_Y), x+o, y+o,0);
				
				x += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("A", screen, PAD_isPressed(BTN_A), x+o, y+o,0);
			}
			
			// VOLUME group
			if (has_volume) {
				int x = (screen->w - SCALE1(99))/2;
				int y = oy + SCALE1(PILL_SIZE);
				int w = SCALE1(42);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(98)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				blitButton("VOL. -", screen, PAD_isPressed(BTN_MINUS), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				blitButton("VOL. +", screen, PAD_isPressed(BTN_PLUS), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
			}
			
			// SYSTEM group
			if (has_power || has_menu) {
				int bw = 42;
				int pw = has_both ? (bw*2 + BUTTON_MARGIN*3) : (bw + BUTTON_MARGIN*2);
				
				int x = (screen->w - SCALE1(pw))/2;
				int y = oy + SCALE1(PILL_SIZE * 3);
				int w = SCALE1(bw);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(pw)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				if (has_menu) {
					blitButton("MENU", screen, PAD_isPressed(BTN_MENU), x, y, w);
					x += w + SCALE1(BUTTON_MARGIN);
				}
				if (has_power) {
					blitButton("POWER", screen, PAD_isPressed(BTN_POWER), x, y, w);
					x += w + SCALE1(BUTTON_MARGIN);
				}
			}
			
			// META group
			{
				int x = (screen->w - SCALE1(99))/2;
				int y = oy + SCALE1(PILL_SIZE * 5);
				int w = SCALE1(42);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(130)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				blitButton("SELECT", screen, PAD_isPressed(BTN_SELECT), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				blitButton("START", screen, PAD_isPressed(BTN_START), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.tiny, "QUIT", uintToColour(THEME_COLOR6_255));
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){x,y+(SCALE1(BUTTON_SIZE)-text->h)/2});
				SDL_FreeSurface(text);
			}
			
			// L3
			if (has_L3) {
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*6);
				int o = SCALE1(BUTTON_MARGIN);
				int travel = o * 2;
				int radius = (SCALE1(PILL_SIZE) + travel) / 2;
				int cx = x + SCALE1(PILL_SIZE) / 2;
				int cy = y + SCALE1(PILL_SIZE) / 2;

				fillCircle(screen, cx, cy, radius, THEME_COLOR2);
				int dx = MAX(-travel, MIN(travel, (pad.laxis.x * travel) / STICK_AXIS_MAX));
				int dy = MAX(-travel, MIN(travel, (pad.laxis.y * travel) / STICK_AXIS_MAX));
				blitButton("L3", screen, PAD_isPressed(BTN_L3), x + o + dx, y + o + dy, 0);
			}
			
			// R3
			if (has_R3) {
				int x = screen->w - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*6);
				int o = SCALE1(BUTTON_MARGIN);
				int travel = o * 2;
				int radius = (SCALE1(PILL_SIZE) + travel) / 2;
				int cx = x + SCALE1(PILL_SIZE) / 2;
				int cy = y + SCALE1(PILL_SIZE) / 2;

				fillCircle(screen, cx, cy, radius, THEME_COLOR2);
				int dx = MAX(-travel, MIN(travel, (pad.raxis.x * travel) / STICK_AXIS_MAX));
				int dy = MAX(-travel, MIN(travel, (pad.raxis.y * travel) / STICK_AXIS_MAX));
				blitButton("R3", screen, PAD_isPressed(BTN_R3), x+o+dx, y+o+dy,0);
			}

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}
	
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	
	return EXIT_SUCCESS;
}

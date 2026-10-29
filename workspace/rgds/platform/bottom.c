// rgds: the launcher's companion on the bottom panel — selected game's box art and name, clock, battery.
// Included by platform.c ahead of generic_video.c, whose SDL_CreateWindow / SDL_GL_SwapWindow calls are
// routed through here so the main UI lands on the top panel and the bottom panel gets redrawn.
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <time.h>

static SDL_Window* bottom_window;
static SDL_Renderer* bottom_renderer;
static TTF_Font* bottom_font_big;
static TTF_Font* bottom_font;
static char bottom_name[256];
static char bottom_art[1024];
static int bottom_dirty = 1;
static time_t bottom_drawn_at;

// KMSDRM numbers displays in connector order: DSI-1 (bottom panel) first, DSI-2 (top) second.
static int topDisplay(void) {
	const char* v = getenv("NEXTUI_TOP_DISPLAY");
	return v ? atoi(v) : 1;
}

void PLAT_selectionChanged(const char* name, const char* art_path) {
	if (strcmp(name, bottom_name) == 0 && strcmp(art_path, bottom_art) == 0) return;
	snprintf(bottom_name, sizeof(bottom_name), "%s", name);
	snprintf(bottom_art, sizeof(bottom_art), "%s", art_path);
	bottom_dirty = 1;
}

static void bottomText(TTF_Font* font, const char* text, SDL_Color color, int cx, int y, int max_w) {
	if (!font || !text[0]) return;
	SDL_Surface* s = TTF_RenderUTF8_Blended(font, text, color);
	if (!s) return;
	SDL_Texture* t = SDL_CreateTextureFromSurface(bottom_renderer, s);
	int w = s->w > max_w ? max_w : s->w;
	SDL_Rect src = {0, 0, w, s->h}, dst = {cx - w / 2, y, w, s->h};
	SDL_RenderCopy(bottom_renderer, t, &src, &dst);
	SDL_DestroyTexture(t);
	SDL_FreeSurface(s);
}

static void bottomDraw(void) {
	time_t now = time(NULL);
	if (!bottom_renderer || (!bottom_dirty && now - bottom_drawn_at < 15)) return;
	bottom_dirty = 0;
	bottom_drawn_at = now;

	int W, H;
	SDL_GetRendererOutputSize(bottom_renderer, &W, &H);
	SDL_SetRenderDrawColor(bottom_renderer, 0, 0, 0, 255);
	SDL_RenderClear(bottom_renderer);

	if (!bottom_font_big) {
		if (!TTF_WasInit()) TTF_Init();
		bottom_font_big = TTF_OpenFont(RES_PATH "/font1.ttf", 64);
		bottom_font = TTF_OpenFont(RES_PATH "/font1.ttf", 26);
	}
	SDL_Color white = {255, 255, 255, 255}, grey = {150, 150, 150, 255};

	char clock[16], date[64], status[64];
	struct tm* tm = localtime(&now);
	strftime(clock, sizeof(clock), "%H:%M", tm);
	strftime(date, sizeof(date), "%A %d %B", tm);
	bottomText(bottom_font_big, clock, white, W / 2, 16, W);
	bottomText(bottom_font, date, grey, W / 2, 92, W);

	int art_bottom = H - 70;
	SDL_Surface* art = bottom_art[0] && exists(bottom_art) ? IMG_Load(bottom_art) : NULL;
	if (art) {
		int max_w = W - 40, max_h = art_bottom - 140;
		float s = SDL_min(max_w / (float)art->w, max_h / (float)art->h);
		SDL_Rect dst = {0, 0, (int)(art->w * s), (int)(art->h * s)};
		dst.x = (W - dst.w) / 2;
		dst.y = 136 + (max_h - dst.h) / 2;
		SDL_Texture* t = SDL_CreateTextureFromSurface(bottom_renderer, art);
		SDL_RenderCopy(bottom_renderer, t, NULL, &dst);
		SDL_DestroyTexture(t);
		SDL_FreeSurface(art);
	}
	bottomText(bottom_font, bottom_name, white, W / 2, art_bottom, W - 20);

	int charging = 0, charge = 0;
	PLAT_getBatteryStatusFine(&charging, &charge);
	snprintf(status, sizeof(status), "%d%%%s", charge, charging ? " charging" : "");
	bottomText(bottom_font, status, grey, W / 2, H - 34, W);

	SDL_RenderPresent(bottom_renderer);
}

static SDL_Window* rgds_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	int top = topDisplay();
	if (SDL_GetNumVideoDisplays() < 2) top = 0;
	SDL_Window* main_window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED_DISPLAY(top), SDL_WINDOWPOS_UNDEFINED_DISPLAY(top), w, h, flags);
	if (main_window && SDL_GetNumVideoDisplays() >= 2 && !bottom_window) {
		int bottom = 1 - top;
		SDL_DisplayMode dm;
		SDL_GetDesktopDisplayMode(bottom, &dm);
		bottom_window = SDL_CreateWindow("NextUI Bottom", SDL_WINDOWPOS_UNDEFINED_DISPLAY(bottom), SDL_WINDOWPOS_UNDEFINED_DISPLAY(bottom),
		                                 dm.w, dm.h, SDL_WINDOW_SHOWN);
		bottom_renderer = bottom_window ? SDL_CreateRenderer(bottom_window, -1, SDL_RENDERER_ACCELERATED) : NULL;
		LOG_info("bottom panel: display %d %dx%d %s\n", bottom, dm.w, dm.h, bottom_renderer ? "ok" : SDL_GetError());
	}
	return main_window;
}

static void rgds_GL_SwapWindow(SDL_Window* window) {
	SDL_GL_SwapWindow(window);
	if (bottom_renderer && (bottom_dirty || time(NULL) - bottom_drawn_at >= 15)) {
		SDL_GLContext ctx = SDL_GL_GetCurrentContext();
		bottomDraw();
		SDL_GL_MakeCurrent(window, ctx); // the main UI keeps its own GL context current
	}
}

static void rgds_DestroyWindow(SDL_Window* window) {
	if (bottom_renderer) { SDL_DestroyRenderer(bottom_renderer); bottom_renderer = NULL; }
	if (bottom_window) { SDL_DestroyWindow(bottom_window); bottom_window = NULL; }
	if (bottom_font_big) { TTF_CloseFont(bottom_font_big); bottom_font_big = NULL; }
	if (bottom_font) { TTF_CloseFont(bottom_font); bottom_font = NULL; }
	SDL_DestroyWindow(window);
}

#define SDL_CreateWindow rgds_CreateWindow
#define SDL_GL_SwapWindow rgds_GL_SwapWindow
#define SDL_DestroyWindow rgds_DestroyWindow

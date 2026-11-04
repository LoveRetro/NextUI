// rgds: the launcher's companion on the bottom panel — the Cocoon-style rail, in the panel you
// touch, while the top panel shows the selected game's art, logo and title.
//
// Lives in platform.c ahead of generic_video.c, whose SDL_CreateWindow / SDL_GL_SwapWindow calls
// are routed through here so the main UI lands on the top panel and the bottom panel gets redrawn.
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <time.h>

static SDL_Window* bottom_window;
static SDL_Renderer* bottom_renderer;
static TTF_Font* bottom_font_title;
static TTF_Font* bottom_font_small;
static TTF_Font* bottom_font_tiny;

// --- Rail data (published by the launcher) -----------------------------------

#define BOTTOM_RAIL_MAX 128
#define BOTTOM_NAME_MAX 96
#define BOTTOM_PATH_MAX 512

static struct {
	char name[BOTTOM_NAME_MAX];
	char art[BOTTOM_PATH_MAX];
	int icon;
} bottom_rail[BOTTOM_RAIL_MAX];
static int bottom_rail_count;
static int bottom_rail_selected;

static char bottom_name[BOTTOM_NAME_MAX];
static char bottom_line1[64];
static char bottom_line2[64];
static int bottom_selection_index = -1;

static int bottom_dirty = 1;
static time_t bottom_drawn_at;

// the selected card's entrance: it overshoots and settles, one step per presented frame
static Uint32 bottom_bounce_at;
static int bottom_bounce_on;

// Test hook: touching /tmp/nextui-shot saves the next frame of each panel to
// /tmp/nextui-shot-top.ppm and /tmp/nextui-shot-bottom.ppm.
static int shot_pending;
static void saveShot(SDL_Renderer* r, const char* path) {
	int w, h;
	if (SDL_GetRendererOutputSize(r, &w, &h) != 0) return;
	Uint8* px = malloc((size_t)w * h * 3);
	if (px && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_RGB24, px, w * 3) == 0) {
		FILE* f = fopen(path, "wb");
		if (f) {
			fprintf(f, "P6 %d %d 255\n", w, h);
			fwrite(px, 3, (size_t)w * h, f);
			fclose(f);
		}
	}
	free(px);
}

// KMSDRM numbers displays in connector order: DSI-1 (bottom panel) first, DSI-2 (top) second.
static int topDisplay(void) {
	const char* v = getenv("NEXTUI_TOP_DISPLAY");
	return v ? atoi(v) : 1;
}

// --- Texture cache ------------------------------------------------------------

#define BOTTOM_TEX_CACHE 6
static struct {
	char* key;
	SDL_Texture* tex;
	int w, h;
} bottom_tex[BOTTOM_TEX_CACHE];
static int bottom_tex_next;

static void bottomTexClear(void) {
	for (int i = 0; i < BOTTOM_TEX_CACHE; i++) {
		if (bottom_tex[i].tex) SDL_DestroyTexture(bottom_tex[i].tex);
		if (bottom_tex[i].key) free(bottom_tex[i].key);
		memset(&bottom_tex[i], 0, sizeof(bottom_tex[i]));
	}
}

static SDL_Texture* bottomTexPut(const char* key, SDL_Surface* s, int w, int h) {
	if (!s) return NULL;
	SDL_Texture* t = SDL_CreateTextureFromSurface(bottom_renderer, s);
	SDL_FreeSurface(s);
	if (!t) return NULL;
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	int slot = bottom_tex_next++ % BOTTOM_TEX_CACHE;
	if (bottom_tex[slot].tex) SDL_DestroyTexture(bottom_tex[slot].tex);
	if (bottom_tex[slot].key) free(bottom_tex[slot].key);
	bottom_tex[slot].tex = t;
	bottom_tex[slot].key = strdup(key);
	bottom_tex[slot].w = w;
	bottom_tex[slot].h = h;
	return t;
}

static SDL_Texture* bottomTexFind(const char* key, int w, int h) {
	for (int i = 0; i < BOTTOM_TEX_CACHE; i++) {
		if (bottom_tex[i].tex && bottom_tex[i].w == w && bottom_tex[i].h == h && bottom_tex[i].key && strcmp(bottom_tex[i].key, key) == 0)
			return bottom_tex[i].tex;
	}
	return NULL;
}

// A card: cover art aspect-filled and rounded, or the system icon centred on a dark card.
static SDL_Texture* bottomCardTexture(const char* path, int icon, int w, int h, int radius) {
	char key[BOTTOM_PATH_MAX + 32];
	snprintf(key, sizeof(key), "%s|%d|%d|%d", path ? path : "", icon, w, h);
	SDL_Texture* hit = bottomTexFind(key, w, h);
	if (hit) return hit;

	SDL_Surface* card = GFX_roundRectSurface(w, h, radius, 26, 27, 32, 255);
	if (card && path && path[0] && exists((char*)path)) {
		SDL_Surface* src = IMG_Load(path);
		SDL_Surface* conv = src ? SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
		if (src) SDL_FreeSurface(src);
		if (conv) {
			if (icon) {
				int target = (int)(w * 0.55f);
				float s = (float)target / (float)(conv->w > conv->h ? conv->w : conv->h);
				int iw = (int)(conv->w * s), ih = (int)(conv->h * s);
				SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, iw, ih, 32, SDL_PIXELFORMAT_ARGB8888);
				if (scaled) {
					SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
					SDL_BlitScaled(conv, NULL, scaled, NULL);
					SDL_SetSurfaceBlendMode(scaled, SDL_BLENDMODE_BLEND);
					SDL_BlitSurface(scaled, NULL, card, &(SDL_Rect){(w - iw) / 2, (h - ih) / 2, 0, 0});
					SDL_FreeSurface(scaled);
				}
			} else {
				GFX_blitScaleToFill(conv, card);
			}
			SDL_FreeSurface(conv);
		}
	}
	if (card && radius > 0) GFX_ApplyRoundedCorners_8888(card, NULL, radius);
	return bottomTexPut(key, card, w, h);
}

static SDL_Texture* bottomRingTexture(int w, int h, int radius, int thickness) {
	char key[64];
	snprintf(key, sizeof(key), "ring|%d|%d|%d|%d", w, h, radius, thickness);
	SDL_Texture* hit = bottomTexFind(key, w, h);
	if (hit) return hit;
	SDL_Surface* ring = GFX_ringSurface(w, h, radius, thickness, 255, 255, 255, 235);
	return bottomTexPut(key, ring, w, h);
}

// --- Published state ----------------------------------------------------------

void PLAT_railChanged(const PLAT_RailItem* items, int count, int selected) {
	if (!items || count < 0) return;
	if (count > BOTTOM_RAIL_MAX) count = BOTTOM_RAIL_MAX;

	int changed = count != bottom_rail_count;
	for (int i = 0; !changed && i < count; i++) {
		if (strcmp(items[i].name ? items[i].name : "", bottom_rail[i].name) != 0 ||
		    strcmp(items[i].art ? items[i].art : "", bottom_rail[i].art) != 0 ||
		    (items[i].icon ? 1 : 0) != bottom_rail[i].icon)
			changed = 1;
	}
	if (!changed && selected == bottom_rail_selected) return;

	for (int i = 0; i < count; i++) {
		snprintf(bottom_rail[i].name, sizeof(bottom_rail[i].name), "%s", items[i].name ? items[i].name : "");
		snprintf(bottom_rail[i].art, sizeof(bottom_rail[i].art), "%s", items[i].art ? items[i].art : "");
		bottom_rail[i].icon = items[i].icon ? 1 : 0;
	}
	bottom_rail_count = count;
	if (selected != bottom_rail_selected) {
		bottom_bounce_at = SDL_GetTicks();
		bottom_bounce_on = 1;
	}
	bottom_rail_selected = selected;
	bottom_dirty = 1;
}

void PLAT_selectionChanged(const PLAT_Selection* sel) {
	if (!sel || !sel->name) return;
	const char* line1 = sel->line1 ? sel->line1 : "";
	const char* line2 = sel->line2 ? sel->line2 : "";
	int same = strcmp(sel->name, bottom_name) == 0 && strcmp(line1, bottom_line1) == 0 &&
	           strcmp(line2, bottom_line2) == 0 && sel->index == bottom_selection_index;
	if (same) return;
	snprintf(bottom_name, sizeof(bottom_name), "%s", sel->name);
	snprintf(bottom_line1, sizeof(bottom_line1), "%s", line1);
	snprintf(bottom_line2, sizeof(bottom_line2), "%s", line2);
	if (sel->index != bottom_selection_index) {
		bottom_bounce_at = SDL_GetTicks();
		bottom_bounce_on = 1;
	}
	bottom_selection_index = sel->index;
	bottom_rail_selected = sel->index;
	bottom_dirty = 1;
}

// --- Text ---------------------------------------------------------------------

static void bottomFonts(void) {
	if (bottom_font_title) return;
	if (!TTF_WasInit()) TTF_Init();
	bottom_font_title = TTF_OpenFont(RES_PATH "/font1.ttf", 32);
	bottom_font_small = TTF_OpenFont(RES_PATH "/font1.ttf", 20);
	bottom_font_tiny = TTF_OpenFont(RES_PATH "/font1.ttf", 16);
}

static void bottomTextLeft(TTF_Font* font, const char* text, SDL_Color color, int x, int y, int max_w) {
	if (!font || !text || !text[0] || max_w <= 0) return;
	SDL_Surface* s = TTF_RenderUTF8_Blended(font, text, color);
	if (!s) return;
	SDL_Texture* t = SDL_CreateTextureFromSurface(bottom_renderer, s);
	if (!t) { SDL_FreeSurface(s); return; }
	int w = s->w > max_w ? max_w : s->w;
	SDL_Rect src = {0, 0, w, s->h}, dst = {x, y, w, s->h};
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_RenderCopy(bottom_renderer, t, &src, &dst);
	SDL_DestroyTexture(t);
	SDL_FreeSurface(s);
}

static void bottomTextCentre(TTF_Font* font, const char* text, SDL_Color color, int cx, int y, int max_w) {
	if (!font || !text || !text[0] || max_w <= 0) return;
	SDL_Surface* s = TTF_RenderUTF8_Blended(font, text, color);
	if (!s) return;
	SDL_Texture* t = SDL_CreateTextureFromSurface(bottom_renderer, s);
	if (!t) { SDL_FreeSurface(s); return; }
	int w = s->w > max_w ? max_w : s->w;
	SDL_Rect src = {0, 0, w, s->h}, dst = {cx - w / 2, y, w, s->h};
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_RenderCopy(bottom_renderer, t, &src, &dst);
	SDL_DestroyTexture(t);
	SDL_FreeSurface(s);
}

// --- Lit button caps ----------------------------------------------------------

static struct { char label[8]; SDL_Texture* tex; int w, h; } bottom_caps[6];
static int bottom_caps_next;

static SDL_Texture* bottomCap(const char* label, int* out_w, int* out_h) {
	*out_w = 0;
	*out_h = 0;
	if (!bottom_font_tiny) return NULL;
	for (int i = 0; i < 6; i++) {
		if (bottom_caps[i].tex && strcmp(bottom_caps[i].label, label) == 0) {
			*out_w = bottom_caps[i].w;
			*out_h = bottom_caps[i].h;
			return bottom_caps[i].tex;
		}
	}
	int tw = 0, th = 0;
	TTF_SizeUTF8(bottom_font_tiny, label, &tw, &th);
	int h = 24, w = (tw + 14 < h ? h : tw + 14);
	SDL_Surface* cap = GFX_roundRectSurface(w, h, h / 2, 242, 242, 245, 255);
	if (!cap) return NULL;
	SDL_Color dark = {11, 11, 15, 255};
	SDL_Surface* text = TTF_RenderUTF8_Blended(bottom_font_tiny, label, dark);
	if (text) {
		SDL_BlitSurface(text, NULL, cap, &(SDL_Rect){(w - text->w) / 2, (h - text->h) / 2, 0, 0});
		SDL_FreeSurface(text);
	}
	SDL_Texture* tex = SDL_CreateTextureFromSurface(bottom_renderer, cap);
	SDL_FreeSurface(cap);
	if (!tex) return NULL;
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	int slot = bottom_caps_next++ % 6;
	if (bottom_caps[slot].tex) SDL_DestroyTexture(bottom_caps[slot].tex);
	bottom_caps[slot].tex = tex;
	snprintf(bottom_caps[slot].label, sizeof(bottom_caps[slot].label), "%s", label);
	bottom_caps[slot].w = w;
	bottom_caps[slot].h = h;
	*out_w = w;
	*out_h = h;
	return tex;
}

static void bottomCapsClear(void) {
	for (int i = 0; i < 6; i++) {
		if (bottom_caps[i].tex) SDL_DestroyTexture(bottom_caps[i].tex);
		memset(&bottom_caps[i], 0, sizeof(bottom_caps[i]));
	}
}

// "A OPEN   B BACK   MENU INFO", each button on a lit cap.
static void bottomDrawHints(int W, int y) {
	static const char* caps[] = {"A", "B", "MENU"};
	static const char* labels[] = {"OPEN", "BACK", "INFO"};
	SDL_Color muted = {158, 162, 172, 255};

	int capw[3] = {0}, caph[3] = {0}, labw[3] = {0}, seg[3] = {0};
	int total = 0;
	for (int i = 0; i < 3; i++) {
		bottomCap(caps[i], &capw[i], &caph[i]);
		int tw = 0, th = 0;
		TTF_SizeUTF8(bottom_font_tiny, labels[i], &tw, &th);
		labw[i] = tw;
		seg[i] = capw[i] + 8 + tw;
		total += seg[i] + (i < 2 ? 28 : 0);
	}
	int x = (W - total) / 2;
	for (int i = 0; i < 3; i++) {
		int cw = 0, ch = 0;
		SDL_Texture* tex = bottomCap(caps[i], &cw, &ch);
		if (tex) {
			SDL_Rect dst = {x, y + (24 - ch) / 2, cw, ch};
			SDL_RenderCopy(bottom_renderer, tex, NULL, &dst);
		}
		bottomTextLeft(bottom_font_tiny, labels[i], muted, x + cw + 8, y + 2, labw[i] + 4);
		x += seg[i] + 28;
	}
}

// --- Rail ---------------------------------------------------------------------

// 1.0 -> 1.14 (overshoot) -> 1.07 -> 1.10 (rest) over 260 ms.
static float bottomSelectedScale(Uint32 now) {
	if (!bottom_bounce_on) return 1.10f;
	float t = (float)(now - bottom_bounce_at) / 260.0f;
	if (t >= 1.0f) {
		bottom_bounce_on = 0;
		return 1.10f;
	}
	if (t < 0.40f) return 1.0f + 0.14f * (t / 0.40f);
	if (t < 0.70f) return 1.14f - 0.07f * ((t - 0.40f) / 0.30f);
	return 1.07f + 0.03f * ((t - 0.70f) / 0.30f);
}

// The rail: three cards, the selected one lifted and ringed, the neighbours dimmed.
static void bottomDrawRail(int W, float sel_scale) {
	int cw = 150, ch = 190, gap = 22;
	int radius = 14, ring = 4;
	int cx0 = W / 2;
	int cy = 178;

	for (int slot = -1; slot <= 1; slot++) {
		if (bottom_rail_count == 0) break;
		int idx = (bottom_rail_selected + slot + bottom_rail_count) % bottom_rail_count;
		int cw_s = cw, ch_s = ch;
		int cx = cx0 + slot * (cw + gap);
		if (slot == 0) {
			cw_s = (int)(cw * sel_scale);
			ch_s = (int)(ch * sel_scale);
			SDL_Texture* ringtex = bottomRingTexture(cw_s + 2 * ring, ch_s + 2 * ring, radius + ring / 2 + 1, ring);
			if (ringtex) {
				SDL_Rect dst = {cx - (cw_s + 2 * ring) / 2, cy - (ch_s + 2 * ring) / 2, cw_s + 2 * ring, ch_s + 2 * ring};
				SDL_RenderCopy(bottom_renderer, ringtex, NULL, &dst);
			}
		}
		SDL_Texture* card = bottomCardTexture(bottom_rail[idx].art, bottom_rail[idx].icon, cw_s, ch_s, radius);
		if (card) {
			SDL_Rect dst = {cx - cw_s / 2, cy - ch_s / 2, cw_s, ch_s};
			SDL_RenderCopy(bottom_renderer, card, NULL, &dst);
		}
		if (slot != 0) {
			SDL_SetRenderDrawColor(bottom_renderer, 0, 0, 0, 110);
			SDL_Rect dst = {cx - cw_s / 2, cy - ch_s / 2, cw_s, ch_s};
			SDL_RenderFillRect(bottom_renderer, &dst);
		}
	}

	if (bottom_rail_count > 1) {
		SDL_Color muted = {154, 160, 170, 220};
		bottomTextLeft(bottom_font_tiny, "\xE2\x80\xB9", muted, 18, cy - 8, 12);
		bottomTextLeft(bottom_font_tiny, "\xE2\x80\xBA", muted, W - 20, cy - 8, 12);
	}
}

static void bottomDraw(void) {
	time_t now = time(NULL);
	if (!bottom_renderer || (!bottom_dirty && now - bottom_drawn_at < 15)) return;
	bottom_dirty = 0;
	bottom_drawn_at = now;

	int W, H;
	SDL_GetRendererOutputSize(bottom_renderer, &W, &H);
	bottomFonts();

	// ground: the Cocoon charcoal, with a soft band at the top for the clock
	SDL_SetRenderDrawColor(bottom_renderer, 11, 11, 15, 255);
	SDL_RenderClear(bottom_renderer);
	for (int y = 0; y < 70; y++) {
		SDL_SetRenderDrawColor(bottom_renderer, 0, 0, 0, (Uint8)(90 - y));
		SDL_RenderDrawLine(bottom_renderer, 0, y, W, y);
	}

	SDL_Color white = {242, 242, 245, 255}, muted = {158, 162, 172, 255};

	char clock[16];
	strftime(clock, sizeof(clock), "%H:%M", localtime(&now));
	bottomTextLeft(bottom_font_small, clock, white, 22, 14, 120);
	int charging = 0, charge = 0;
	PLAT_getBatteryStatusFine(&charging, &charge);
	char status[48];
	snprintf(status, sizeof(status), "%d%%%s", charge, charging ? " +" : "");
	bottomTextLeft(bottom_font_tiny, status, muted, W - 82, 18, 70);
	if (bottom_rail_count > 1) {
		char counter[48];
		snprintf(counter, sizeof(counter), "%d of %d", bottom_rail_selected + 1, bottom_rail_count);
		bottomTextCentre(bottom_font_tiny, counter, muted, W / 2, 18, 160);
	}

	bottomDrawRail(W, bottomSelectedScale(SDL_GetTicks()));

	// title and the quiet lines under the rail
	int ty = 306;
	bottomTextCentre(bottom_font_title, bottom_name, white, W / 2, ty, W - 80);
	ty += 42;
	bottomTextCentre(bottom_font_small, bottom_line1, muted, W / 2, ty, W - 80);
	if (bottom_line2[0]) {
		ty += 28;
		bottomTextCentre(bottom_font_tiny, bottom_line2, muted, W / 2, ty, W - 80);
	}

	// hints on the panel that gets touched, with the buttons lit so the mapping is obvious
	SDL_SetRenderDrawColor(bottom_renderer, 255, 255, 255, 24);
	SDL_RenderDrawLine(bottom_renderer, 32, H - 56, W - 32, H - 56);
	bottomDrawHints(W, H - 46);

	if (shot_pending) {
		saveShot(bottom_renderer, "/tmp/nextui-shot-bottom.ppm");
		shot_pending = 0;
	}
	SDL_RenderPresent(bottom_renderer);

	// keep presenting while the card is still settling
	if (bottom_bounce_on) bottom_dirty = 1;
}

static SDL_Window* rgds_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	int top = topDisplay();
	if (SDL_GetNumVideoDisplays() < 2) top = 0;
	SDL_Window* main_window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED_DISPLAY(top), SDL_WINDOWPOS_UNDEFINED_DISPLAY(top), w, h, flags);
	if (main_window) {
		int ww = 0, wh = 0;
		SDL_GetWindowSize(main_window, &ww, &wh);
		SDL_Renderer* r = SDL_GetRenderer(main_window);
		int rw = 0, rh = 0;
		if (r) SDL_GetRendererOutputSize(r, &rw, &rh);
		SDL_DisplayMode dm;
		SDL_GetDesktopDisplayMode(top, &dm);
		LOG_info("top panel: window %dx%d renderer %dx%d display %dx%d\n", ww, wh, rw, rh, dm.w, dm.h);
	}
	if (main_window && SDL_GetNumVideoDisplays() >= 2 && !bottom_window) {
		int bottom = 1 - top;
		SDL_DisplayMode dm;
		SDL_GetDesktopDisplayMode(bottom, &dm);
		bottom_window = SDL_CreateWindow("NextUI Bottom", SDL_WINDOWPOS_UNDEFINED_DISPLAY(bottom), SDL_WINDOWPOS_UNDEFINED_DISPLAY(bottom),
		                                 dm.w, dm.h, SDL_WINDOW_SHOWN);
		bottom_renderer = bottom_window ? SDL_CreateRenderer(bottom_window, -1, SDL_RENDERER_ACCELERATED) : NULL;
		if (bottom_renderer) SDL_SetRenderDrawBlendMode(bottom_renderer, SDL_BLENDMODE_BLEND);
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

// The launcher presents its menus with SDL_RenderPresent (games/shaders use the GL swap above).
static void rgds_RenderPresent(SDL_Renderer* renderer) {
	static int logged = 0;
	if (!logged && renderer != bottom_renderer) {
		int rw = 0, rh = 0, ww = 0, wh = 0;
		SDL_GetRendererOutputSize(renderer, &rw, &rh);
		SDL_Window* win = SDL_GL_GetCurrentWindow();
		if (win) SDL_GetWindowSize(win, &ww, &wh);
		LOG_info("top panel present: renderer %dx%d window %dx%d\n", rw, rh, ww, wh);
		logged = 1;
	}
	if (renderer != bottom_renderer && unlink("/tmp/nextui-shot") == 0) {
		saveShot(renderer, "/tmp/nextui-shot-top.ppm");
		shot_pending = bottom_dirty = 1;
	}
	SDL_RenderPresent(renderer);
	if (renderer != bottom_renderer && bottom_renderer && (bottom_dirty || time(NULL) - bottom_drawn_at >= 15)) {
		SDL_GLContext ctx = SDL_GL_GetCurrentContext();
		SDL_Window* win = SDL_GL_GetCurrentWindow();
		bottomDraw();
		if (ctx && win) SDL_GL_MakeCurrent(win, ctx);
	}
}

static void rgds_DestroyWindow(SDL_Window* window) {
	if (bottom_renderer) { SDL_DestroyRenderer(bottom_renderer); bottom_renderer = NULL; }
	if (bottom_window) { SDL_DestroyWindow(bottom_window); bottom_window = NULL; }
	bottomTexClear();
	bottomCapsClear();
	if (bottom_font_title) { TTF_CloseFont(bottom_font_title); bottom_font_title = NULL; }
	if (bottom_font_small) { TTF_CloseFont(bottom_font_small); bottom_font_small = NULL; }
	if (bottom_font_tiny) { TTF_CloseFont(bottom_font_tiny); bottom_font_tiny = NULL; }
	SDL_DestroyWindow(window);
}

#define SDL_CreateWindow rgds_CreateWindow
#define SDL_GL_SwapWindow rgds_GL_SwapWindow
#define SDL_DestroyWindow rgds_DestroyWindow
#define SDL_RenderPresent rgds_RenderPresent

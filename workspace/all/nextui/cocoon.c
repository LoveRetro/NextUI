// Cocoon HOME for NextUI Duo (rgds / Anbernic RG DS), laid out like the dual-screen Cocoon HOME:
// the top panel is the showcase (full-bleed art, logo and title of the selection), the bottom panel
// is the one you touch (the rail of cover-art cards, the title and the hints).
//
// The look follows Calamansi's Cocoon HOME (PR #20, Cocoon 3.11 RE notes in
// evidence/cocoon-ui-re-20261103). Included by nextui.c after its globals, so it uses `screen`,
// `top`, `stack` and `font` directly; everything is drawn on the CPU surface `screen`.

#ifndef COCOON_C
#define COCOON_C

#include <sqlite3.h>
#include <gametimedb.h>

// --- Cocoon palette (the style C dark HOME) ---------------------------------
#define COCOON_BG_R 11
#define COCOON_BG_G 11
#define COCOON_BG_B 15
#define COCOON_TEXT_R 242
#define COCOON_TEXT_G 242
#define COCOON_TEXT_B 245
#define COCOON_MUTED_R 154
#define COCOON_MUTED_G 160
#define COCOON_MUTED_B 170

// --- Layout (logical units, scaled with SCALE1) ------------------------------
#define COCOON_TITLE_Y 186	// title top, logical (lower third)

// --- Small surface helpers ---------------------------------------------------

static SDL_Surface* cocoonSurface(int w, int h) {
	if (w <= 0 || h <= 0) return NULL;
	SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (s) SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);
	return s;
}

static void cocoonBlit(SDL_Surface* src, int x, int y) {
	if (!src) return;
	SDL_BlitSurface(src, NULL, screen, &(SDL_Rect){x, y, 0, 0});
}

// Centred text, clipped to max_w.
static void cocoonText(TTF_Font* f, const char* str, SDL_Color color, int cx, int y, int max_w) {
	if (!f || !str || !str[0] || max_w <= 0) return;
	SDL_LockMutex(fontMutex);
	SDL_Surface* text = TTF_RenderUTF8_Blended(f, str, color);
	SDL_UnlockMutex(fontMutex);
	if (!text) return;
	int w = text->w > max_w ? max_w : text->w;
	int x = cx - w / 2;
	if (x < 0) x = 0;
	SDL_Rect src = {0, 0, w, text->h};
	SDL_BlitSurface(text, &src, screen, &(SDL_Rect){x, y, 0, 0});
	SDL_FreeSurface(text);
}

static void cocoonTextLeft(TTF_Font* f, const char* str, SDL_Color color, int x, int y, int max_w) {
	if (!f || !str || !str[0] || max_w <= 0) return;
	SDL_LockMutex(fontMutex);
	SDL_Surface* text = TTF_RenderUTF8_Blended(f, str, color);
	SDL_UnlockMutex(fontMutex);
	if (!text) return;
	int w = text->w > max_w ? max_w : text->w;
	SDL_Rect src = {0, 0, w, text->h};
	SDL_BlitSurface(text, &src, screen, &(SDL_Rect){x, y, 0, 0});
	SDL_FreeSurface(text);
}

static void cocoonTextRight(TTF_Font* f, const char* str, SDL_Color color, int right_x, int y, int max_w) {
	if (!f || !str || !str[0] || max_w <= 0) return;
	SDL_LockMutex(fontMutex);
	SDL_Surface* text = TTF_RenderUTF8_Blended(f, str, color);
	SDL_UnlockMutex(fontMutex);
	if (!text) return;
	int w = text->w > max_w ? max_w : text->w;
	SDL_Rect src = {0, 0, w, text->h};
	int x = right_x - w;
	if (x < 0) x = 0;
	SDL_BlitSurface(text, &src, screen, &(SDL_Rect){x, y, 0, 0});
	SDL_FreeSurface(text);
}

// --- Paths -------------------------------------------------------------------

// .media/<stem><suffix>.png next to the rom
static void cocoonMediaPath(const Entry* entry, const char* suffix, char* out, size_t out_size) {
	out[0] = '\0';
	char tmp[MAX_PATH];
	snprintf(tmp, sizeof(tmp), "%s", entry->path);
	char* base = strrchr(tmp, '/');
	if (!base) return;
	*base = '\0';
	char stem[MAX_PATH];
	snprintf(stem, sizeof(stem), "%s", base + 1);
	char* dot = strrchr(stem, '.');
	if (dot) *dot = '\0';
	snprintf(out, out_size, "%s/.media/%s%s.png", tmp, stem, suffix);
}

// First box art inside a folder, used as the folder's own tile (Cocoon has console/home art there).
static int cocoonFirstMedia(const char* dir, char* out, size_t out_size) {
	char media[MAX_PATH];
	snprintf(media, sizeof(media), "%s/.media", dir);
	DIR* d = opendir(media);
	if (!d) return 0;
	struct dirent* e;
	char best[256] = "";
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".png") != 0) continue;
		if (strstr(e->d_name, "-hero") || strstr(e->d_name, "-logo")) continue;
		if (!strcmp(e->d_name, "bg.png") || !strcmp(e->d_name, "bglist.png") || !strcmp(e->d_name, "quick.png")) continue;
		if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof(best), "%s", e->d_name);
	}
	closedir(d);
	if (!best[0]) return 0;
	snprintf(out, out_size, "%s/%s", media, best);
	return 1;
}

// Art that stands for an entry: cover art for a rom, the folder's own art for a directory.
static void cocoonEntryArt(const Entry* entry, char* out, size_t out_size) {
	out[0] = '\0';
	if (!entry) return;
	if (entry->type == ENTRY_ROM) {
		cocoonMediaPath(entry, "", out, out_size);
		return;
	}
	if (entry->type == ENTRY_DIR) {
		char bg[MAX_PATH];
		snprintf(bg, sizeof(bg), "%s/.media/bg.png", entry->path);
		if (exists(bg)) {
			snprintf(out, out_size, "%s", bg);
			return;
		}
		cocoonFirstMedia(entry->path, out, out_size);
		return;
	}
}

// System icon for a folder or tool that has no art of its own.
static void cocoonIconPath(const Entry* entry, char* out, size_t out_size) {
	out[0] = '\0';
	if (!entry) return;
	char candidate[MAX_PATH];
	snprintf(candidate, sizeof(candidate), RES_PATH "/%s@%ix.png", entry->name, FIXED_SCALE);
	if (exists(candidate)) { snprintf(out, out_size, "%s", candidate); return; }
	const char* aliases[] = {"Recent", "Collection", "Tool", "Game"};
	const char* icons[] = {"Recents", "Collections", "Tools", "Games"};
	for (int i = 0; i < 4; i++) {
		if (strstr(entry->name, aliases[i])) {
			snprintf(out, out_size, RES_PATH "/%s@%ix.png", icons[i], FIXED_SCALE);
			return;
		}
	}
}

// The backdrop source: scraped hero art, else the cover art / folder art.
static void cocoonBackdropPath(const Entry* entry, char* out, size_t out_size) {
	out[0] = '\0';
	if (!entry) return;
	if (entry->type == ENTRY_ROM) {
		cocoonMediaPath(entry, "-hero", out, out_size);
		if (exists(out)) return;
		cocoonMediaPath(entry, "", out, out_size);
		return;
	}
	if (entry->type == ENTRY_DIR) {
		char bg[MAX_PATH];
		snprintf(bg, sizeof(bg), "%s/.media/bg.png", entry->path);
		if (exists(bg)) {
			snprintf(out, out_size, "%s", bg);
			return;
		}
	}
	snprintf(out, out_size, SDCARD_PATH "/bg.png");
	if (!exists(out)) out[0] = '\0';
}

// --- Backdrop ----------------------------------------------------------------

static char cocoon_bg_path[MAX_PATH];
static SDL_Surface* cocoon_bg_surf = NULL;

// Cover-fill `src` into a w x h surface (crop, no letterbox).
static SDL_Surface* cocoonFill(SDL_Surface* src, int w, int h) {
	SDL_Surface* dst = cocoonSurface(w, h);
	if (!dst) return NULL;
	GFX_blitScaleToFill(src, dst);
	return dst;
}

// A vertical grade over the backdrop: dark at the top and bottom, lighter in the middle band,
// so the pill, the logo and the title always read.
static void cocoonGrade(int w, int h) {
	SDL_Surface* grade = cocoonSurface(w, h);
	if (!grade) return;
	SDL_FillRect(grade, NULL, SDL_MapRGBA(grade->format, 0, 0, 0, 0));
	for (int y = 0; y < h; y++) {
		float t = (float)y / (float)h;
		int a;
		if (t < 0.28f) a = (int)(236.0f * (1.0f - t / 0.28f) + 36.0f);			// top shade
		else if (t < 0.58f) a = 36;												// middle
		else a = 36 + (int)(186.0f * ((t - 0.58f) / 0.42f));					// bottom shade
		SDL_FillRect(grade, &(SDL_Rect){0, y, w, 1}, SDL_MapRGBA(grade->format, 0, 0, 0, a));
	}
	SDL_BlitSurface(grade, NULL, screen, NULL);
	SDL_FreeSurface(grade);
}

static void cocoonDrawBackdrop(const Entry* entry) {
	char path[MAX_PATH];
	cocoonBackdropPath(entry, path, sizeof(path));
	if (strcmp(path, cocoon_bg_path) != 0) {
		if (cocoon_bg_surf) {
			SDL_FreeSurface(cocoon_bg_surf);
			cocoon_bg_surf = NULL;
		}
		snprintf(cocoon_bg_path, sizeof(cocoon_bg_path), "%s", path);
		if (path[0]) {
			SDL_Surface* src = IMG_Load(path);
			if (src) {
				SDL_Surface* conv = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
				SDL_FreeSurface(src);
				if (conv) {
					SDL_Surface* filled = cocoonFill(conv, screen->w, screen->h);
					SDL_FreeSurface(conv);
					// wide hero art fills the panel almost 1:1; a 2:3 cover has to be enlarged, so soften it
					if (filled && !strstr(path, "-hero")) GFX_boxBlur(filled, 5);
					cocoon_bg_surf = filled;
				}
			}
		}
	}
	if (cocoon_bg_surf) {
		SDL_SetClipRect(screen, NULL);
		SDL_SetSurfaceBlendMode(cocoon_bg_surf, SDL_BLENDMODE_NONE);
		SDL_BlitScaled(cocoon_bg_surf, NULL, screen, NULL);
		SDL_SetSurfaceBlendMode(cocoon_bg_surf, SDL_BLENDMODE_BLEND);
	}
	else {
		SDL_FillRect(screen, NULL, SDL_MapRGBA(screen->format, COCOON_BG_R, COCOON_BG_G, COCOON_BG_B, 255));
	}
	cocoonGrade(screen->w, screen->h);
}

// Trim the sorting metadata NextUI keeps on display names the way the list does.
static void cocoonDisplayName(Entry* entry, char* out, size_t out_size) {
	char* name = entry->unique ? entry->unique : entry->name;
	snprintf(out, out_size, "%s", name);
	char* p = out;
	trimSortingMeta(&p);
	memmove(out, p, strlen(p) + 1);
}

// --- Selection animation ------------------------------------------------------

// After a selection change the launcher redraws a few extra frames: the logo fades in and the
// bottom panel's rail bounces (it animates on its own clock, one step per presented frame).
static int cocoon_anim;
static char cocoon_anim_key[MAX_PATH];

static void cocoonAnimStart(const char* key) {
	cocoon_anim = 6;
	snprintf(cocoon_anim_key, sizeof(cocoon_anim_key), "%s", key ? key : "");
}

int cocoonFrameWanted(void) {
	return cocoon_anim > 0;
}

static int cocoonAnimStep(void) {
	if (cocoon_anim <= 0) return 0;
	cocoon_anim--;
	return cocoon_anim;
}

// --- Top panel: the showcase -------------------------------------------------

void cocoonDraw(Entry* selected_entry, int total, int show_setting) {
	int i = top->selected;
	// start from opaque black: the stream layer is composited premultiplied, so a stray transparent
	// pixel would show whatever the layer below still holds
	SDL_FillRect(screen, NULL, SDL_MapRGBA(screen->format, 0, 0, 0, 255));
	cocoonDrawBackdrop(selected_entry);

	// header: the open folder, quiet and small, top-left
	{
		char label[256] = "";
		if (stack->count > 1) {
			snprintf(label, sizeof(label), "%s", top->path);
			char* base = strrchr(label, '/');
			if (base) memmove(label, base + 1, strlen(base));
			char* paren = strstr(label, " (");
			if (paren) *paren = '\0';
		}
		SDL_Color muted = (SDL_Color){COCOON_MUTED_R, COCOON_MUTED_G, COCOON_MUTED_B, 255};
		cocoonTextLeft(font.small, label, muted, SCALE1(PADDING) + SCALE1(3), SCALE1(PADDING) + SCALE1(5), screen->w / 2);
	}

	// a selection change starts the little entrance animation
	static char last_key[MAX_PATH];
	if (selected_entry && strcmp(selected_entry->path, last_key) != 0) {
		snprintf(last_key, sizeof(last_key), "%s", selected_entry->path);
		cocoonAnimStart(last_key);
	}
	int anim_step = cocoonAnimStep();
	int anim_alpha = 255 - (int)(255.0f * (float)anim_step / 6.0f);

	if (total > 0 && selected_entry) {
		char name[256];
		cocoonDisplayName(selected_entry, name, sizeof(name));
		for (char* p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

		int title_y = SCALE1(COCOON_TITLE_Y);

		// No hero art yet: keep the box art legible, centred over the blurred ground.
		char hero[MAX_PATH] = "";
		cocoonMediaPath(selected_entry, "-hero", hero, sizeof(hero));
		if (!hero[0] || !exists(hero)) {
			char art[MAX_PATH] = "";
			cocoonEntryArt(selected_entry, art, sizeof(art));
			if (art[0] && exists(art)) {
				SDL_Surface* src = IMG_Load(art);
				SDL_Surface* conv = src ? SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
				if (src) SDL_FreeSurface(src);
				if (conv) {
					int max_w = (int)(screen->w * 0.60f), max_h = (int)(screen->h * 0.60f);
					float s = (float)max_w / (float)conv->w;
					if (conv->h * s > max_h) s = (float)max_h / (float)conv->h;
					int w = (int)(conv->w * s), h = (int)(conv->h * s);
					SDL_Surface* scaled = cocoonSurface(w, h);
					if (scaled) {
						SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
						SDL_BlitScaled(conv, NULL, scaled, NULL);
						SDL_SetSurfaceBlendMode(scaled, SDL_BLENDMODE_BLEND);
						int ay = title_y - SCALE1(14) - h;
						if (ay < SCALE1(PADDING * 3)) ay = SCALE1(PADDING * 3);
						SDL_Surface* shadow = GFX_roundRectSurface(w + SCALE1(6), h + SCALE1(6), SCALE1(4), 0, 0, 0, 110);
						if (shadow) {
							cocoonBlit(shadow, (screen->w - w) / 2 + SCALE1(3), ay + SCALE1(4));
							SDL_FreeSurface(shadow);
						}
						cocoonBlit(scaled, (screen->w - w) / 2, ay);
						SDL_FreeSurface(scaled);
					}
					SDL_FreeSurface(conv);
				}
			}
		}

		// the game logo above the title, when the scraper found one
		char logo[MAX_PATH] = "";
		cocoonMediaPath(selected_entry, "-logo", logo, sizeof(logo));
		if (logo[0] && exists(logo)) {
			SDL_Surface* src = IMG_Load(logo);
			if (src) {
				SDL_Surface* conv = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
				SDL_FreeSurface(src);
				if (conv) {
					int max_w = (int)(screen->w * 0.56f), max_h = SCALE1(58);
					float s = (float)max_w / (float)conv->w;
					if (conv->h * s > max_h) s = (float)max_h / (float)conv->h;
					int w = (int)(conv->w * s), h = (int)(conv->h * s);
					SDL_Surface* scaled = cocoonSurface(w, h);
					if (scaled) {
						SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
						SDL_BlitScaled(conv, NULL, scaled, NULL);
						SDL_SetSurfaceBlendMode(scaled, SDL_BLENDMODE_BLEND);
						// fades in over the entrance frames, and rises a few pixels into place
						int rise = (int)((float)SCALE1(8) * (float)anim_step / 6.0f);
						SDL_SetSurfaceAlphaMod(scaled, (Uint8)anim_alpha);
						cocoonBlit(scaled, (screen->w - w) / 2, title_y - h - SCALE1(6) + rise);
						SDL_FreeSurface(scaled);
					}
					SDL_FreeSurface(conv);
				}
			}
		}

		SDL_Color text = (SDL_Color){COCOON_TEXT_R, COCOON_TEXT_G, COCOON_TEXT_B, 255};
		char fitted[256];
		GFX_truncateText(font.large, name, fitted, screen->w - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
		cocoonText(font.large, fitted, text, screen->w / 2, title_y, screen->w - SCALE1(PADDING * 4));

		char counter[32];
		snprintf(counter, sizeof(counter), "%d of %d", i + 1, total);
		SDL_Color muted = (SDL_Color){COCOON_MUTED_R, COCOON_MUTED_G, COCOON_MUTED_B, 255};
		cocoonTextRight(font.small, counter, muted, screen->w - SCALE1(PADDING),
			SCALE1(PADDING + PILL_SIZE + 8), SCALE1(80));
	}

	// the status pill goes on last so the backdrop never covers it
	GFX_blitHardwareGroup(screen, show_setting);

	// The compositor leaves the top few rows of the panel showing the previous buffer (an RG DS
	// KMSDRM quirk); a solid Cocoon band hides it and reads as a status bar.
	SDL_FillRect(screen, &(SDL_Rect){0, 0, screen->w, SCALE1(7)}, SDL_MapRGBA(screen->format, COCOON_BG_R, COCOON_BG_G, COCOON_BG_B, 255));

	// the overlay layers belong to the list UI; make sure nothing stale sits above the art
	GFX_clearLayers(LAYER_THUMBNAIL);
	GFX_clearLayers(LAYER_SCROLLTEXT);
	GFX_clearLayers(LAYER_IDK2);

}

// An empty folder still gets the Cocoon ground.
void cocoonDrawEmpty(int show_setting) {
	SDL_FillRect(screen, NULL, SDL_MapRGBA(screen->format, COCOON_BG_R, COCOON_BG_G, COCOON_BG_B, 255));
	SDL_Color muted = (SDL_Color){COCOON_MUTED_R, COCOON_MUTED_G, COCOON_MUTED_B, 255};
	cocoonText(font.large, "Empty", muted, screen->w / 2, screen->h / 2 - SCALE1(FONT_LARGE), screen->w - SCALE1(PADDING * 4));
	GFX_blitHardwareGroup(screen, show_setting);
}

// Playtime for a rom, read once per selection from the gametime database (its paths are relative
// to ROMS_PATH).
static void cocoonPlayStats(const char* rom_path, char* out, size_t out_size) {
	out[0] = '\0';
	if (!rom_path || !rom_path[0]) return;
	static char have_path[MAX_PATH];
	static char have_text[64];
	if (strcmp(rom_path, have_path) == 0) {
		snprintf(out, out_size, "%s", have_text);
		return;
	}
	snprintf(have_path, sizeof(have_path), "%s", rom_path);
	have_text[0] = '\0';

	const char* rel = rom_path;
	size_t n = strlen(ROMS_PATH);
	if (strncmp(rel, ROMS_PATH, n) == 0) {
		rel += n;
		while (*rel == '/') rel++;
	}
	PlayActivities* all = play_activity_find_all();
	if (all) {
		for (int i = 0; i < all->count; i++) {
			PlayActivity* pa = all->play_activity[i];
			if (!pa || !pa->rom || !pa->rom->file_path) continue;
			if (strcmp(pa->rom->file_path, rel) != 0) continue;
			int s = pa->play_time_total;
			if (s < 60) snprintf(have_text, sizeof(have_text), "Played less than a minute");
			else if (s < 3600) snprintf(have_text, sizeof(have_text), "Played %d min", s / 60);
			else snprintf(have_text, sizeof(have_text), "Played %dh %02dm", s / 3600, (s % 3600) / 60);
			break;
		}
		free_play_activities(all);
	}
	snprintf(out, out_size, "%s", have_text);
}

// Tell the bottom panel what is selected: art, hero, logo and the quiet lines.
void cocoonPublishSelection(Entry* entry) {
	if (!entry) return;

	PLAT_Selection sel;
	memset(&sel, 0, sizeof(sel));
	char name[256];
	char art[MAX_PATH], hero[MAX_PATH], logo[MAX_PATH], line1[64], line2[64];
	name[0] = art[0] = hero[0] = logo[0] = line1[0] = line2[0] = '\0';

	cocoonDisplayName(entry, name, sizeof(name));
	cocoonEntryArt(entry, art, sizeof(art));
	if (!exists(art)) art[0] = '\0';
	cocoonMediaPath(entry, "-hero", hero, sizeof(hero));
	if (!exists(hero)) hero[0] = '\0';
	cocoonMediaPath(entry, "-logo", logo, sizeof(logo));
	if (!exists(logo)) logo[0] = '\0';

	if (entry->type == ENTRY_ROM) {
		// "Roms/Nintendo DS (NDS)/Game.nds" -> "Nintendo DS"
		char romdir[MAX_PATH];
		snprintf(romdir, sizeof(romdir), "%s", entry->path);
		char* slash = strrchr(romdir, '/');
		if (slash) *slash = '\0';
		char* base = strrchr(romdir, '/');
		if (base) base++;
		else base = romdir;
		snprintf(line1, sizeof(line1), "%s", base);
		char* paren = strstr(line1, " (");
		if (paren) *paren = '\0';
		cocoonPlayStats(entry->path, line2, sizeof(line2));
		if (!line2[0]) snprintf(line2, sizeof(line2), "Not played yet");
	}
	else if (entry->type == ENTRY_DIR) {
		snprintf(line1, sizeof(line1), "Folder");
		if (entry->path && strcmp(entry->path, top->path) == 0)
			snprintf(line2, sizeof(line2), "%d items", top->entries->count);
	}
	else {
		snprintf(line1, sizeof(line1), "App");
	}

	sel.name = name;
	sel.art = art;
	sel.hero = hero;
	sel.logo = logo;
	sel.line1 = line1;
	sel.line2 = line2;
	sel.index = top->selected;
	PLAT_selectionChanged(&sel);
}

// The rail lives on the bottom panel: publish the folder's entries whenever it changes.
#define COCOON_RAIL_MAX 128
void cocoonPublishRail(void) {
	if (!top || !top->entries) return;
	int n = top->entries->count;
	if (n > COCOON_RAIL_MAX) n = COCOON_RAIL_MAX;

	static char names[COCOON_RAIL_MAX][96];
	static char arts[COCOON_RAIL_MAX][MAX_PATH];
	static PLAT_RailItem items[COCOON_RAIL_MAX];
	for (int i = 0; i < n; i++) {
		Entry* e = top->entries->items[i];
		cocoonDisplayName(e, names[i], sizeof(names[i]));
		cocoonEntryArt(e, arts[i], sizeof(arts[i]));
		items[i].name = names[i];
		items[i].art = arts[i];
		items[i].icon = 0;
		if (!arts[i][0] || !exists(arts[i])) {
			char icon[MAX_PATH] = "";
			cocoonIconPath(e, icon, sizeof(icon));
			snprintf(arts[i], sizeof(arts[i]), "%s", icon);
			items[i].icon = icon[0] ? 1 : 0;
		}
	}
	PLAT_railChanged(items, n, top->selected);
}

#endif

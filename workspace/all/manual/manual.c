#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "manual.h"

static ManualHost g_manual_host = {0};

void Manual_setHost(const ManualHost* host) {
	if (host) g_manual_host = *host;
	else memset(&g_manual_host, 0, sizeof(g_manual_host));
}

static SDL_Surface* Manual_getScreen(void) {
	if (!g_manual_host.screen) return NULL;
	return *g_manual_host.screen;
}

static int Manual_shouldQuit(void) {
	if (!g_manual_host.quit) return 0;
	return *g_manual_host.quit;
}

static void Manual_startFrame(void) {
	if (g_manual_host.start_frame) g_manual_host.start_frame();
}

static void Manual_pollInput(void) {
	if (g_manual_host.poll_input) g_manual_host.poll_input();
}

static void Manual_resetInput(void) {
	if (g_manual_host.reset_input) g_manual_host.reset_input();
}

static int Manual_justPressed(ManualButton button) {
	if (!g_manual_host.just_pressed) return 0;
	return g_manual_host.just_pressed(button);
}

static int Manual_isPressed(ManualButton button) {
	if (!g_manual_host.is_pressed) return 0;
	return g_manual_host.is_pressed(button);
}

static void Manual_powerUpdate(int* dirty) {
	if (g_manual_host.power_update) g_manual_host.power_update(dirty);
}

static void Manual_clear(SDL_Surface* screen) {
	if (!screen) return;
	if (g_manual_host.clear) {
		g_manual_host.clear(screen);
		return;
	}
	SDL_FillRect(screen, NULL, 0);
}

static void Manual_flip(SDL_Surface* screen) {
	if (!screen) return;
	if (g_manual_host.flip) g_manual_host.flip(screen);
}

static void Manual_delay(void) {
	if (g_manual_host.delay) g_manual_host.delay();
}

static void Manual_drawNotice(SDL_Surface* screen, const char* msg) {
	if (g_manual_host.draw_notice) g_manual_host.draw_notice(screen, msg);
}

static void Manual_drawOverlay(SDL_Surface* screen, const char* page_info) {
	if (g_manual_host.draw_overlay) g_manual_host.draw_overlay(screen, page_info);
}

static int Manual_fileExists(const char* path) {
	FILE* fp = fopen(path, "rb");
	if (!fp) return 0;
	fclose(fp);
	return 1;
}

static void Manual_showMessage(const char* msg) {
	SDL_Surface* screen = Manual_getScreen();
	if (!screen || !msg) return;

	Manual_clear(screen);
	Manual_drawNotice(screen, msg);
	Manual_flip(screen);
}

static void Manual_waitForDismiss(void) {
	Manual_resetInput();
	while (!Manual_shouldQuit()) {
		Manual_startFrame();
		Manual_pollInput();
		if (Manual_justPressed(MANUAL_BTN_A) || Manual_justPressed(MANUAL_BTN_B)) break;
		Manual_powerUpdate(NULL);
		Manual_delay();
	}
}

#define MANUAL_MAX_PATH 4096

static int Manual_findLocalPath(const char* rom_path, char* out_path,
                                size_t out_sz) {
	if (!rom_path || !rom_path[0] || !out_path || out_sz == 0) return 0;

	char rom_dir[MANUAL_MAX_PATH];
	strncpy(rom_dir, rom_path, sizeof(rom_dir));
	rom_dir[sizeof(rom_dir) - 1] = '\0';

	char* slash = strrchr(rom_dir, '/');
	if (!slash) return 0;

	char rom_file[MANUAL_MAX_PATH];
	strncpy(rom_file, slash + 1, sizeof(rom_file));
	rom_file[sizeof(rom_file) - 1] = '\0';
	*slash = '\0';

	char* dot = strrchr(rom_file, '.');
	if (dot) *dot = '\0';

	snprintf(out_path, out_sz, "%s/.manuals/%s.pdf", rom_dir, rom_file);
	if (Manual_fileExists(out_path)) return 1;

	snprintf(out_path, out_sz, "%s/.manuals/%s.PDF", rom_dir, rom_file);
	if (Manual_fileExists(out_path)) return 1;

	out_path[0] = '\0';
	return 0;
}

#ifdef ENABLE_PDF_MANUAL
#include <mupdf/fitz.h>

#define MANUAL_ZOOM_STEP 1.05f
#define MANUAL_MIN_SCALE 0.10f
#define MANUAL_MAX_SCALE 6.00f
#define MANUAL_PAN_STEP 20.0f
#define MANUAL_VIEW_EPSILON 0.01f

typedef struct {
	fz_context* ctx;
	fz_document* doc;
	int page_count;
	int current_page;
	int view_align_right;
	float page_w;
	float page_h;
	float scale;
	float x_offset;
	float y_offset;
} ManualState;

static ManualState manual = {0};

static float Manual_clampf(float v, float lo, float hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static float Manual_fitHeightScale(SDL_Surface* screen) {
	if (!screen || manual.page_h <= 0) return MANUAL_MIN_SCALE;
	return Manual_clampf((float)screen->h / manual.page_h,
	                     MANUAL_MIN_SCALE, MANUAL_MAX_SCALE);
}

static int Manual_maxOffsetX(SDL_Surface* screen) {
	if (!screen) return 0;
	int scaled_w = (int)(manual.page_w * manual.scale);
	int max_x = scaled_w - screen->w;
	return max_x > 0 ? max_x : 0;
}

static int Manual_maxOffsetY(SDL_Surface* screen) {
	if (!screen) return 0;
	int scaled_h = (int)(manual.page_h * manual.scale);
	int max_y = scaled_h - screen->h;
	return max_y > 0 ? max_y : 0;
}

static int Manual_updatePageMetrics(void) {
	if (!manual.ctx || !manual.doc) return 0;

	fz_page* page = NULL;
	fz_try(manual.ctx) {
		page = fz_load_page(manual.ctx, manual.doc, manual.current_page);
		fz_rect bounds = fz_bound_page(manual.ctx, page);
		manual.page_w = bounds.x1 - bounds.x0;
		manual.page_h = bounds.y1 - bounds.y0;
	}
	fz_catch(manual.ctx) {
		manual.page_w = 0;
		manual.page_h = 0;
	}

	if (page) fz_drop_page(manual.ctx, page);
	return (manual.page_w > 0 && manual.page_h > 0);
}

static void Manual_resetView(SDL_Surface* screen, int align_right) {
	if (!screen || manual.page_h <= 0) return;

	manual.scale = Manual_fitHeightScale(screen);
	manual.y_offset = 0;
	manual.view_align_right = align_right ? 1 : 0;

	int max_x = Manual_maxOffsetX(screen);
	manual.x_offset = align_right ? (float)max_x : 0.0f;
}

static int Manual_isAtDefaultView(SDL_Surface* screen) {
	if (!screen) return 1;
	float expected_x = manual.view_align_right ? (float)Manual_maxOffsetX(screen) : 0.0f;
	float x_diff = manual.x_offset - expected_x;
	if (x_diff < 0) x_diff = -x_diff;
	if (x_diff > MANUAL_VIEW_EPSILON) return 0;
	if (manual.y_offset > MANUAL_VIEW_EPSILON) return 0;
	float base_scale = Manual_fitHeightScale(screen);
	float scale_diff = manual.scale - base_scale;
	if (scale_diff < 0) scale_diff = -scale_diff;
	return scale_diff <= MANUAL_VIEW_EPSILON;
}

static int Manual_isZoomedIn(SDL_Surface* screen) {
	if (!screen) return 0;
	float base_scale = Manual_fitHeightScale(screen);
	return manual.scale > (base_scale + MANUAL_VIEW_EPSILON);
}

static void Manual_render(void) {
	SDL_Surface* screen = Manual_getScreen();
	if (!screen || !manual.ctx || !manual.doc) return;

	fz_page* page = NULL;
	fz_pixmap* pix = NULL;

	fz_try(manual.ctx) { page = fz_load_page(manual.ctx, manual.doc, manual.current_page); }
	fz_catch(manual.ctx) { return; }

	fz_matrix ctm = fz_scale(manual.scale, manual.scale);
	fz_try(manual.ctx) {
		pix = fz_new_pixmap_from_page(manual.ctx, page, ctm, fz_device_rgb(manual.ctx), 0);
	}
	fz_catch(manual.ctx) {
		fz_drop_page(manual.ctx, page);
		return;
	}

	int w = fz_pixmap_width(manual.ctx, pix);
	int h = fz_pixmap_height(manual.ctx, pix);
	int stride = fz_pixmap_stride(manual.ctx, pix);
	unsigned char* samples = fz_pixmap_samples(manual.ctx, pix);

	Manual_clear(screen);

	SDL_Surface* page_surf = SDL_CreateRGBSurfaceWithFormatFrom(
	    samples, w, h, 24, stride, SDL_PIXELFORMAT_RGB24);
	if (page_surf) {
		SDL_Rect src = {0, 0, w, h};
		SDL_Rect dst = {0, 0, w, h};

		if (w > screen->w) {
			int max_x = w - screen->w;
			manual.x_offset = Manual_clampf(manual.x_offset, 0.0f, (float)max_x);
			src.x = (int)manual.x_offset;
			src.w = screen->w;
			dst.x = 0;
		} else {
			src.x = 0;
			src.w = w;
			dst.x = (screen->w - w) / 2;
			manual.x_offset = 0;
		}

		if (h > screen->h) {
			int max_y = h - screen->h;
			manual.y_offset = Manual_clampf(manual.y_offset, 0.0f, (float)max_y);
			src.y = (int)manual.y_offset;
			src.h = screen->h;
			dst.y = 0;
		} else {
			src.y = 0;
			src.h = h;
			dst.y = (screen->h - h) / 2;
			manual.y_offset = 0;
		}

		dst.w = src.w;
		dst.h = src.h;
		SDL_BlitSurface(page_surf, &src, screen, &dst);

		SDL_FreeSurface(page_surf);
	}

	char page_info[64];
	snprintf(page_info, sizeof(page_info), "%d / %d", manual.current_page + 1,
	         manual.page_count);
	Manual_drawOverlay(screen, page_info);
	Manual_flip(screen);

	fz_drop_pixmap(manual.ctx, pix);
	fz_drop_page(manual.ctx, page);
}

static void Manual_loop(const char* pdf_path) {
	SDL_Surface* screen = Manual_getScreen();
	if (!screen || !pdf_path) return;

	manual.ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!manual.ctx) return;

	fz_register_document_handlers(manual.ctx);

	fz_try(manual.ctx) { manual.doc = fz_open_document(manual.ctx, pdf_path); }
	fz_catch(manual.ctx) {
		fz_drop_context(manual.ctx);
		manual.ctx = NULL;
		return;
	}

	fz_try(manual.ctx) { manual.page_count = fz_count_pages(manual.ctx, manual.doc); }
	fz_catch(manual.ctx) { manual.page_count = 0; }

	if (manual.page_count <= 0 || !Manual_updatePageMetrics()) {
		fz_drop_document(manual.ctx, manual.doc);
		fz_drop_context(manual.ctx);
		memset(&manual, 0, sizeof(manual));
		return;
	}

	manual.current_page = 0;
	Manual_resetView(screen, 0);

	int dirty = 1;
	Manual_resetInput();

	while (!Manual_shouldQuit()) {
		Manual_startFrame();
		Manual_pollInput();

		if (Manual_justPressed(MANUAL_BTN_B)) {
			if (Manual_isAtDefaultView(screen)) break;
			Manual_resetView(screen, manual.view_align_right);
			dirty = 1;
			continue;
		}

		if (Manual_isZoomedIn(screen)) {
			int max_x = Manual_maxOffsetX(screen);
			if (Manual_isPressed(MANUAL_BTN_RIGHT)) {
				manual.x_offset += MANUAL_PAN_STEP;
				if (manual.x_offset > (float)max_x) manual.x_offset = (float)max_x;
				dirty = 1;
			} else if (Manual_isPressed(MANUAL_BTN_LEFT)) {
				manual.x_offset -= MANUAL_PAN_STEP;
				if (manual.x_offset < 0.0f) manual.x_offset = 0.0f;
				dirty = 1;
			}
		} else if (Manual_justPressed(MANUAL_BTN_RIGHT)) {
			int max_x = Manual_maxOffsetX(screen);
			if (manual.x_offset < (float)max_x) {
				manual.x_offset += screen->w * 0.90f;
				if (manual.x_offset > (float)max_x) manual.x_offset = (float)max_x;
				dirty = 1;
			} else if (manual.current_page < manual.page_count - 1) {
				manual.current_page++;
				if (Manual_updatePageMetrics()) {
					Manual_resetView(screen, 0);
					dirty = 1;
				}
			}
		} else if (Manual_justPressed(MANUAL_BTN_LEFT)) {
			if (manual.x_offset > 0.0f) {
				manual.x_offset -= screen->w * 0.90f;
				if (manual.x_offset < 0.0f) manual.x_offset = 0.0f;
				dirty = 1;
			} else if (manual.current_page > 0) {
				manual.current_page--;
				if (Manual_updatePageMetrics()) {
					Manual_resetView(screen, 1);
					dirty = 1;
				}
			}
		}

		if (Manual_isPressed(MANUAL_BTN_DOWN)) {
			int max_y = Manual_maxOffsetY(screen);
			if (max_y > 0) {
				manual.y_offset += MANUAL_PAN_STEP;
				if (manual.y_offset > (float)max_y) manual.y_offset = (float)max_y;
				dirty = 1;
			}
		} else if (Manual_isPressed(MANUAL_BTN_UP)) {
			if (manual.y_offset > 0.0f) {
				manual.y_offset -= MANUAL_PAN_STEP;
				if (manual.y_offset < 0.0f) manual.y_offset = 0.0f;
				dirty = 1;
			}
		} else if (Manual_isPressed(MANUAL_BTN_R1)) {
			float old_scale = manual.scale;
			float cx = manual.x_offset + (screen->w * 0.5f);
			float cy = manual.y_offset + (screen->h * 0.5f);

			manual.scale = Manual_clampf(manual.scale * MANUAL_ZOOM_STEP,
			                             MANUAL_MIN_SCALE, MANUAL_MAX_SCALE);
			if (manual.scale != old_scale) {
				float s = manual.scale / old_scale;
				manual.x_offset = (cx * s) - (screen->w * 0.5f);
				manual.y_offset = (cy * s) - (screen->h * 0.5f);
				dirty = 1;
			}
		} else if (Manual_isPressed(MANUAL_BTN_L1)) {
			float old_scale = manual.scale;
			float cx = manual.x_offset + (screen->w * 0.5f);
			float cy = manual.y_offset + (screen->h * 0.5f);

			manual.scale = Manual_clampf(manual.scale / MANUAL_ZOOM_STEP,
			                             MANUAL_MIN_SCALE, MANUAL_MAX_SCALE);
			if (manual.scale != old_scale) {
				float s = manual.scale / old_scale;
				manual.x_offset = (cx * s) - (screen->w * 0.5f);
				manual.y_offset = (cy * s) - (screen->h * 0.5f);
				dirty = 1;
			}
		}

		manual.x_offset = Manual_clampf(manual.x_offset, 0.0f,
		                                (float)Manual_maxOffsetX(screen));
		manual.y_offset = Manual_clampf(manual.y_offset, 0.0f,
		                                (float)Manual_maxOffsetY(screen));

		Manual_powerUpdate(&dirty);

		if (dirty) {
			Manual_render();
			dirty = 0;
		} else {
			Manual_delay();
		}
	}

	fz_drop_document(manual.ctx, manual.doc);
	fz_drop_context(manual.ctx);
	memset(&manual, 0, sizeof(manual));
}
#endif

void Manual_open(const char* rom_path) {
	if (!rom_path || !rom_path[0]) return;

	SDL_Surface* screen = Manual_getScreen();
	if (!screen) return;

	char manual_path[MANUAL_MAX_PATH] = {0};
	if (!Manual_findLocalPath(rom_path, manual_path, sizeof(manual_path))) {
		Manual_showMessage("No local manual found.\nPlace PDF in .manuals next to ROM.");
		Manual_waitForDismiss();
		return;
	}

#ifdef ENABLE_PDF_MANUAL
	Manual_loop(manual_path);
#else
	Manual_showMessage("Manual support is disabled in this build.");
	Manual_waitForDismiss();
#endif
}

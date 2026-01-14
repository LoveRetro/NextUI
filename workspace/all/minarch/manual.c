#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include <glib/poppler.h>
#include <cairo.h>
#include <SDL2/SDL.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "manual.h"

// External variables from minarch.c
extern SDL_Surface* screen;
extern int quit;
void Menu_beforeSleep();
void Menu_afterSleep();

typedef struct {
    PopplerDocument *doc;
    int page_count;
    int current_page;
    double scale;
    double x_offset;
    double y_offset;
    int rotation; // 0, 90, 180, 270
} ManualState;

static ManualState manual = {0};

static void Manual_render(void) {
    if (!manual.doc) return;

    PopplerPage *page = poppler_document_get_page(manual.doc, manual.current_page);
    if (!page) return;

    double width, height;
    poppler_page_get_size(page, &width, &height);

    // Calculate scale to fit width if scale is 0 (initial)
    if (manual.scale == 0) {
        manual.scale = (double)screen->w / width;
        // Optionally fit height if it fits better?
        // For reading, width fit is usually better.
    }

    int scaled_w = (int)(width * manual.scale);
    int scaled_h = (int)(height * manual.scale);

    // Create cairo surface for the page
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, scaled_w, scaled_h);
    cairo_t *cr = cairo_create(surface);

    // Fill white background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Apply scale and render
    cairo_scale(cr, manual.scale, manual.scale);
    poppler_page_render(page, cr);

    cairo_destroy(cr);
    g_object_unref(page);

    // Blit to SDL screen
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    // Cairo uses ARGB (premultiplied), SDL uses whatever screen is set to (usually RGB565 or ARGB8888)
    // Assuming screen is 32-bit ARGB or RGBA for now based on minarch.c code

    SDL_Surface *page_surf = SDL_CreateRGBSurfaceWithFormatFrom(data, scaled_w, scaled_h, 32, stride, SDL_PIXELFORMAT_ARGB8888);

    // Clear screen
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 20, 20, 20));

    // Center horizontally if smaller than screen
    SDL_Rect dst_rect;
    dst_rect.x = (screen->w - scaled_w) / 2;
    if (dst_rect.x < 0) dst_rect.x = 0; // Should handle panning later

    // Vertical position based on offset
    dst_rect.y = (int)manual.y_offset; // Simple scrolling
    dst_rect.w = scaled_w;
    dst_rect.h = scaled_h;

    // Handle panning logic for blit
    SDL_Rect src_rect;
    src_rect.x = 0;
    src_rect.y = 0;
    src_rect.w = scaled_w;
    src_rect.h = scaled_h;

    // Adjust for horizontal scrolling
    if (scaled_w > screen->w) {
        src_rect.x = (int)manual.x_offset;
        if (src_rect.x < 0) src_rect.x = 0;
        if (src_rect.x > scaled_w - screen->w) src_rect.x = scaled_w - screen->w;
        dst_rect.x = 0;
        src_rect.w = screen->w;
    }

    // Adjust for vertical scrolling
    if (dst_rect.y < 0) {
        src_rect.y = -dst_rect.y;
        dst_rect.y = 0;
        if (src_rect.y > scaled_h - screen->h) src_rect.y = scaled_h - screen->h;
        src_rect.h = screen->h;
    }

    SDL_BlitSurface(page_surf, &src_rect, screen, &dst_rect);
    SDL_FreeSurface(page_surf);
    cairo_surface_destroy(surface);

    // Render Overlay Info (Page number)
    char info[64];
    snprintf(info, sizeof(info), "Page %d / %d", manual.current_page + 1, manual.page_count);
    // Assuming GFX functions are available or we use basic SDL rendering
    // For now, let's just rely on the visual page content
}

static void Manual_loop(char* pdf_path) {
    // Load PDF
    char uri[MAX_PATH + 7]; // file:// + path
    if (pdf_path[0] == '/')
        snprintf(uri, sizeof(uri), "file://%s", pdf_path);
    else {
        char cwd[MAX_PATH];
        getcwd(cwd, sizeof(cwd));
        snprintf(uri, sizeof(uri), "file://%s/%s", cwd, pdf_path);
    }

    GError *error = NULL;
    manual.doc = poppler_document_new_from_file(uri, NULL, &error);
    if (!manual.doc) {
        LOG_error("Failed to open PDF: %s\n", error->message);
        g_error_free(error);
        return;
    }

    manual.page_count = poppler_document_get_n_pages(manual.doc);
    manual.current_page = 0;
    manual.scale = 0; // Auto-fit width
    manual.x_offset = 0;
    manual.y_offset = 0;

    int show_manual = 1;
    int dirty = 1;

    PAD_reset();

    while (show_manual && !quit) {
        GFX_startFrame();
        PAD_poll();

        if (PAD_justPressed(BTN_B)) {
            show_manual = 0;
        }
        else if (PAD_justPressed(BTN_RIGHT)) {
            if (manual.current_page < manual.page_count - 1) {
                manual.current_page++;
                manual.y_offset = 0;
                dirty = 1;
            }
        }
        else if (PAD_justPressed(BTN_LEFT)) {
            if (manual.current_page > 0) {
                manual.current_page--;
                manual.y_offset = 0;
                dirty = 1;
            }
        }
        else if (PAD_isPressed(BTN_DOWN)) {
             manual.y_offset -= 10; // Scroll down moves content up
             dirty = 1;
        }
        else if (PAD_isPressed(BTN_UP)) {
             manual.y_offset += 10;
             if (manual.y_offset > 0) manual.y_offset = 0;
             dirty = 1;
        }
        else if (PAD_isPressed(BTN_R1)) { // Zoom in
            manual.scale *= 1.02;
            dirty = 1;
        }
        else if (PAD_isPressed(BTN_L1)) { // Zoom out
            manual.scale /= 1.02;
            dirty = 1;
        }

        // PWR update (handle sleep etc)
        int show_setting = 0; // Dummy
        PWR_update(&dirty, &show_setting, Menu_beforeSleep, Menu_afterSleep);

        if (dirty) {
            GFX_clear(screen);
            Manual_render();
            GFX_flip(screen);
            dirty = 0;
        } else {
             GFX_delay();
        }
    }

    g_object_unref(manual.doc);
    manual.doc = NULL;
}

void Manual_open(char* rom_path) {
    char manual_dir[MAX_PATH];
    char* tmp = strrchr(rom_path, '/');
    if (!tmp) return;

    int len = tmp - rom_path;
    strncpy(manual_dir, rom_path, len);
    manual_dir[len] = '\0';
    strcat(manual_dir, "/manuals");

    if (!exists(manual_dir)) {
        // Try to verify if we are in "Roms/<System>/manuals" vs "Roms/<System>"
        // If rom_path is ".../Roms/GBA/game.gba", then dir is ".../Roms/GBA"
        // And manual dir is ".../Roms/GBA/manuals"
        LOG_info("Manual dir not found: %s\n", manual_dir);
        // Maybe try listing files?
        return;
    }

    // List PDFs
    DIR *d;
    struct dirent *dir;
    d = opendir(manual_dir);
    if (!d) return;

    char *pdf_files[64];
    int count = 0;

    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) {
             if (suffixMatch(".pdf", dir->d_name)) {
                 pdf_files[count] = strdup(dir->d_name);
                 count++;
                 if (count >= 64) break;
             }
        }
    }
    closedir(d);

    if (count == 0) {
        // No manuals
        return;
    }

    // If only one manual, open it directly?
    // Or check if one matches the ROM name?
    char rom_name[MAX_PATH];
    getDisplayName(rom_path, rom_name);

    char best_match[MAX_PATH] = {0};
    int found_match = 0;

    // Simple matching: looks for manual with same basename
    for (int i=0; i<count; i++) {
        char manual_name[MAX_PATH];
        getDisplayName(pdf_files[i], manual_name);
        if (exactMatch(rom_name, manual_name)) {
            snprintf(best_match, sizeof(best_match), "%s/%s", manual_dir, pdf_files[i]);
            found_match = 1;
            break;
        }
    }

    if (!found_match && count > 0) {
        // Just take the first one? Or implement a menu to select?
        // For now, let's take the first one if only one, or show a list if multiple.
        if (count == 1) {
             snprintf(best_match, sizeof(best_match), "%s/%s", manual_dir, pdf_files[0]);
             found_match = 1;
        } else {
            // TODO: Implement selection menu.
            // For now, just pick the first one to prove concept.
             snprintf(best_match, sizeof(best_match), "%s/%s", manual_dir, pdf_files[0]);
             found_match = 1;
        }
    }

    if (found_match) {
        Manual_loop(best_match);
    }

    // Cleanup
    for (int i=0; i<count; i++) free(pdf_files[i]);
}

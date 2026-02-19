#include "notification.h"
#include "defines.h"
#include "api.h"
#include "config.h"
#include <string.h>
#include <stdio.h>

///////////////////////////////
// Layout constants (unscaled)
///////////////////////////////

#define NOTIF_PADDING_X      8   // Horizontal padding inside pill
#define NOTIF_PADDING_Y      4   // Vertical padding inside pill
#define NOTIF_MARGIN        12   // Margin from screen edge
#define NOTIF_STACK_GAP      6   // Gap between stacked notifications
#define NOTIF_ICON_GAP       4   // Gap between icon and text
#define NOTIF_SLIDE_IN_MS   180  // Slide-in animation duration
#define NOTIF_FADE_OUT_MS   250  // Fade-out animation duration

// System indicator sizing (must match GFX_blitHardwareIndicator dimensions)
#define SYS_INDICATOR_EXTRA_PAD  4   // Extra padding for indicator pill

///////////////////////////////
// Internal state
///////////////////////////////

static Notification notifications[NOTIFICATION_MAX_QUEUE];
static int notification_count = 0;
static int initialized = 0;

// Persistent surface for GL rendering
static SDL_Surface* gl_notification_surface = NULL;
static int needs_clear_frame = 0;

// Screen dimensions for layer rendering
static int screen_width = 0;
static int screen_height = 0;

// Visual constants (will be set after init with proper scaling)
static int notif_padding_x;
static int notif_padding_y;
static int notif_margin;
static int notif_stack_gap;
static int notif_icon_gap;

// Track if we need to re-render when static state changes
static int render_dirty = 1;
static int last_notification_count = 0;

///////////////////////////////
// System indicator state
///////////////////////////////

static SystemIndicatorType system_indicator_type = SYSTEM_INDICATOR_NONE;
static uint32_t system_indicator_start_time = 0;
static int system_indicator_dirty = 0;
static int last_system_indicator_type = SYSTEM_INDICATOR_NONE;

///////////////////////////////
// Progress indicator state
///////////////////////////////

#define PROGRESS_TITLE_MAX 48
#define PROGRESS_STRING_MAX 16

typedef struct {
    char title[PROGRESS_TITLE_MAX];
    char progress[PROGRESS_STRING_MAX];
    SDL_Surface* icon;
    uint32_t start_time;
    int active;
    int dirty;
    int persistent;
} ProgressIndicatorState;

static ProgressIndicatorState progress_state = {0};

///////////////////////////////
// Rounded rectangle drawing
///////////////////////////////

// Draw a filled rounded rectangle directly to RGBA pixel buffer.
// This is separate from GFX_blitPill* functions in api.c because:
// 1. Notifications render to an RGBA surface for GL overlay compositing
// 2. GFX_blitPill* use pre-made theme assets requiring screen format surfaces
// 3. Direct pixel manipulation avoids format conversion overhead during animation
static void draw_rounded_rect(SDL_Surface* surface, int x, int y, int w, int h, int radius, Uint32 color) {
    if (!surface || w <= 0 || h <= 0) return;
    
    // Clamp radius to half the smallest dimension
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    
    Uint32* pixels = (Uint32*)surface->pixels;
    int pitch = surface->pitch / 4; // pitch in pixels (32-bit)
    
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int draw = 1;
            
            // Check corners
            if (px < radius && py < radius) {
                // Top-left corner
                int dx = radius - px - 1;
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py < radius) {
                // Top-right corner
                int dx = px - (w - radius);
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px < radius && py >= h - radius) {
                // Bottom-left corner
                int dx = radius - px - 1;
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py >= h - radius) {
                // Bottom-right corner
                int dx = px - (w - radius);
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            }
            
            if (draw) {
                pixels[(y + py) * pitch + (x + px)] = color;
            }
        }
    }
}

///////////////////////////////
// Internal helpers
///////////////////////////////

static void remove_notification(int index) {
    if (index < 0 || index >= notification_count) return;
    
    // Shift remaining notifications down
    for (int i = index; i < notification_count - 1; i++) {
        notifications[i] = notifications[i + 1];
    }
    notification_count--;
    render_dirty = 1;
}

static int notification_is_animating(const Notification* n, uint32_t now) {
    uint32_t elapsed = now - n->start_time;
    if (elapsed < NOTIF_SLIDE_IN_MS) return 1;
    if (elapsed > n->duration_ms && elapsed < (n->duration_ms + NOTIF_FADE_OUT_MS)) return 1;
    return 0;
}

static Uint8 notification_alpha(const Notification* n, uint32_t now) {
    uint32_t elapsed = now - n->start_time;
    if (elapsed <= n->duration_ms) return 255;

    uint32_t fade_elapsed = elapsed - n->duration_ms;
    if (fade_elapsed >= NOTIF_FADE_OUT_MS) return 0;

    return (Uint8)(((NOTIF_FADE_OUT_MS - fade_elapsed) * 255U) / NOTIF_FADE_OUT_MS);
}

///////////////////////////////
// Public API
///////////////////////////////

void Notification_init(void) {
    notification_count = 0;
    memset(notifications, 0, sizeof(notifications));
    
    // Initialize scaled visual constants (compact pills)
    notif_padding_x = SCALE1(NOTIF_PADDING_X);
    notif_padding_y = SCALE1(NOTIF_PADDING_Y);
    notif_margin = SCALE1(NOTIF_MARGIN);
    notif_stack_gap = SCALE1(NOTIF_STACK_GAP);
    notif_icon_gap = SCALE1(NOTIF_ICON_GAP);
    
    // Store screen dimensions for layer rendering
    screen_width = FIXED_WIDTH;
    screen_height = FIXED_HEIGHT;
    
    render_dirty = 1;
    last_notification_count = 0;
    initialized = 1;
}

void Notification_push(NotificationType type, const char* message, SDL_Surface* icon) {
    if (!initialized) {
        return;
    }
    
    // Check if notifications are enabled for this type
    if (type == NOTIFICATION_ACHIEVEMENT && !CFG_getRAShowNotifications()) {
        return;
    }
    
    // If queue is full, remove oldest notification
    if (notification_count >= NOTIFICATION_MAX_QUEUE) {
        remove_notification(0);
    }
    
    // Add new notification at end of queue
    Notification* n = &notifications[notification_count];
    n->type = type;
    strncpy(n->message, message, NOTIFICATION_MAX_MESSAGE - 1);
    n->message[NOTIFICATION_MAX_MESSAGE - 1] = '\0';
    n->icon = icon;
    n->start_time = SDL_GetTicks();
    
    // Use RA-specific duration for achievement notifications
    if (type == NOTIFICATION_ACHIEVEMENT) {
        n->duration_ms = CFG_getRANotificationDuration() * 1000;
    } else {
        n->duration_ms = CFG_getNotifyDuration() * 1000;
    }
    n->state = NOTIFICATION_STATE_VISIBLE;
    
    notification_count++;
    render_dirty = 1;
}

void Notification_update(uint32_t now) {
    if (!initialized) return;
    
    // Update system indicator timeout
    if (system_indicator_type != SYSTEM_INDICATOR_NONE) {
        uint32_t elapsed = now - system_indicator_start_time;
        if (elapsed >= SYSTEM_INDICATOR_DURATION_MS) {
            system_indicator_type = SYSTEM_INDICATOR_NONE;
            system_indicator_dirty = 1;
        }
    }
    
    // Update progress indicator timeout (skip if persistent)
    if (progress_state.active && !progress_state.persistent) {
        uint32_t elapsed = now - progress_state.start_time;
        int duration_seconds = CFG_getRAProgressNotificationDuration();
        if (duration_seconds > 0 && elapsed >= (uint32_t)(duration_seconds * 1000)) {
            progress_state.active = 0;
            progress_state.dirty = 1;
        }
    }
    
    // Check each notification for expiration
    for (int i = 0; i < notification_count; i++) {
        Notification* n = &notifications[i];
        uint32_t elapsed = now - n->start_time;
        
        if (n->state == NOTIFICATION_STATE_VISIBLE && elapsed >= (n->duration_ms + NOTIF_FADE_OUT_MS)) {
            n->state = NOTIFICATION_STATE_DONE;
        }
    }
    
    // Remove completed notifications (iterate backwards to avoid index issues)
    for (int i = notification_count - 1; i >= 0; i--) {
        if (notifications[i].state == NOTIFICATION_STATE_DONE) {
            remove_notification(i);
        }
    }
}

// Render system indicator (top-right)
static void render_system_indicator(void) {
    int indicator_width = SCALE1(PILL_SIZE + SETTINGS_WIDTH + PADDING + SYS_INDICATOR_EXTRA_PAD);
    int indicator_height = SCALE1(PILL_SIZE);
    int indicator_x = screen_width - SCALE1(PADDING) - indicator_width;
    int indicator_y = SCALE1(PADDING);
    
    // Create a temporary surface with the SAME format as gfx.screen
    // This is critical because theme colors (THEME_COLOR2, etc.) were mapped
    // using SDL_MapRGB(gfx.screen->format, ...), so they only work correctly
    // on surfaces with that same pixel format.
    SDL_Surface* indicator_surface = GFX_createScreenFormatSurface(indicator_width, indicator_height);
    if (indicator_surface) {
        SDL_FillRect(indicator_surface, NULL, 0);
        GFX_blitHardwareIndicator(indicator_surface, 0, 0, (IndicatorType)system_indicator_type);
        
        // Convert to RGBA for the notification overlay
        SDL_Surface* converted = SDL_ConvertSurfaceFormat(indicator_surface, SDL_PIXELFORMAT_ABGR8888, 0);
        if (converted) {
            SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
            SDL_Rect dst_rect = {indicator_x, indicator_y, indicator_width, indicator_height};
            SDL_BlitSurface(converted, NULL, gl_notification_surface, &dst_rect);
            SDL_FreeSurface(converted);
        }
        SDL_FreeSurface(indicator_surface);
    }
}

// Render progress indicator pill (top-left)
static void render_progress_indicator(void) {
    SDL_Color text_color = uintToColour(THEME_COLOR1_255);
    SDL_Color bg_color_sdl = uintToColour(THEME_COLOR2_255);
    
    // Format: "Title: Progress" or just "Title"
    char progress_text[PROGRESS_TITLE_MAX + PROGRESS_STRING_MAX + 4];
    if (progress_state.progress[0] != '\0') {
        snprintf(progress_text, sizeof(progress_text), "%s: %s", 
                 progress_state.title, progress_state.progress);
    } else {
        snprintf(progress_text, sizeof(progress_text), "%s", progress_state.title);
    }
    
    int text_w = 0, text_h = 0;
    TTF_SizeUTF8(font.tiny, progress_text, &text_w, &text_h);
    
    // Calculate icon dimensions if present
    int icon_w = 0, icon_h = 0, icon_total_w = 0;
    if (progress_state.icon) {
        icon_h = text_h;
        icon_w = (progress_state.icon->w * icon_h) / progress_state.icon->h;
        icon_total_w = icon_w + notif_icon_gap;
    }
    
    int pill_w = icon_total_w + text_w + (notif_padding_x * 2);
    int pill_h = text_h + (notif_padding_y * 2);
    int corner_radius = pill_h / 2;
    int x = notif_margin;
    int y = notif_margin;
    
    SDL_Surface* progress_surface = SDL_CreateRGBSurfaceWithFormat(
        0, pill_w, pill_h, 32, SDL_PIXELFORMAT_ABGR8888
    );
    if (!progress_surface) return;
    
    SDL_FillRect(progress_surface, NULL, 0);
    Uint32 bg_color = SDL_MapRGBA(progress_surface->format, 
                                  bg_color_sdl.r, bg_color_sdl.g, bg_color_sdl.b, 255);
    draw_rounded_rect(progress_surface, 0, 0, pill_w, pill_h, corner_radius, bg_color);
    
    int content_x = notif_padding_x;
    
    if (progress_state.icon && icon_w > 0 && icon_h > 0) {
        SDL_Rect icon_dst = {content_x, notif_padding_y, icon_w, icon_h};
        SDL_SetSurfaceBlendMode(progress_state.icon, SDL_BLENDMODE_BLEND);
        SDL_BlitScaled(progress_state.icon, NULL, progress_surface, &icon_dst);
        content_x += icon_total_w;
    }
    
    SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.tiny, progress_text, text_color);
    if (text_surf) {
        SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_BLEND);
        SDL_Rect text_dst = {content_x, notif_padding_y, text_surf->w, text_surf->h};
        SDL_BlitSurface(text_surf, NULL, progress_surface, &text_dst);
        SDL_FreeSurface(text_surf);
    }
    
    SDL_SetSurfaceBlendMode(progress_surface, SDL_BLENDMODE_NONE);
    SDL_Rect dst_rect = {x, y, pill_w, pill_h};
    SDL_BlitSurface(progress_surface, NULL, gl_notification_surface, &dst_rect);
    SDL_FreeSurface(progress_surface);
}

// Render a single notification pill
static void render_notification_pill(Notification* n, int x, int y, Uint8 alpha, SDL_Color text_color, SDL_Color bg_color_sdl) {
    if (alpha == 0) return;

    int text_w = 0, text_h = 0;
    TTF_SizeUTF8(font.tiny, n->message, &text_w, &text_h);
    
    int icon_w = 0, icon_h = 0, icon_total_w = 0;
    if (n->icon) {
        icon_h = text_h;
        icon_w = (n->icon->w * icon_h) / n->icon->h;
        icon_total_w = icon_w + notif_icon_gap;
    }
    
    int pill_w = icon_total_w + text_w + (notif_padding_x * 2);
    int pill_h = text_h + (notif_padding_y * 2);
    int corner_radius = pill_h / 2;
    
    SDL_Surface* notif_surface = SDL_CreateRGBSurfaceWithFormat(
        0, pill_w, pill_h, 32, SDL_PIXELFORMAT_ABGR8888
    );
    if (!notif_surface) return;
    
    SDL_FillRect(notif_surface, NULL, 0);
    Uint32 bg_color = SDL_MapRGBA(notif_surface->format, bg_color_sdl.r, bg_color_sdl.g, bg_color_sdl.b, 255);
    draw_rounded_rect(notif_surface, 0, 0, pill_w, pill_h, corner_radius, bg_color);
    
    int content_x = notif_padding_x;
    
    if (n->icon && icon_w > 0 && icon_h > 0) {
        SDL_Rect icon_dst = {content_x, notif_padding_y, icon_w, icon_h};
        SDL_SetSurfaceBlendMode(n->icon, SDL_BLENDMODE_BLEND);
        SDL_BlitScaled(n->icon, NULL, notif_surface, &icon_dst);
        content_x += icon_total_w;
    }
    
    SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font.tiny, n->message, text_color);
    if (text_surf) {
        SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_BLEND);
        SDL_Rect text_dst = {content_x, notif_padding_y, text_surf->w, text_surf->h};
        SDL_BlitSurface(text_surf, NULL, notif_surface, &text_dst);
        SDL_FreeSurface(text_surf);
    }
    
    if (alpha < 255) {
        SDL_SetSurfaceBlendMode(notif_surface, SDL_BLENDMODE_BLEND);
        SDL_SetSurfaceAlphaMod(notif_surface, alpha);
    } else {
        SDL_SetSurfaceBlendMode(notif_surface, SDL_BLENDMODE_NONE);
    }
    SDL_Rect dst_rect = {x, y, pill_w, pill_h};
    SDL_BlitSurface(notif_surface, NULL, gl_notification_surface, &dst_rect);
    SDL_FreeSurface(notif_surface);
}

// Render notification stack (bottom-left, stacking upward)
static void render_notification_stack(uint32_t now) {
    SDL_Color text_color = uintToColour(THEME_COLOR1_255);
    SDL_Color bg_color_sdl = uintToColour(THEME_COLOR2_255);
    
    int base_x = notif_margin;
    int base_y = screen_height - notif_margin;
    
    for (int i = 0; i < notification_count; i++) {
        Notification* n = &notifications[i];
        
        int text_w = 0, text_h = 0;
        TTF_SizeUTF8(font.tiny, n->message, &text_w, &text_h);

        int icon_w = 0, icon_h = 0, icon_total_w = 0;
        if (n->icon) {
            icon_h = text_h;
            icon_w = (n->icon->w * icon_h) / n->icon->h;
            icon_total_w = icon_w + notif_icon_gap;
        }

        int pill_w = icon_total_w + text_w + (notif_padding_x * 2);
        int pill_h = text_h + (notif_padding_y * 2);
        
        // Calculate stack offset (how far up from base)
        int stack_offset = 0;
        for (int j = i + 1; j < notification_count; j++) {
            int other_text_h = 0;
            TTF_SizeUTF8(font.tiny, notifications[j].message, NULL, &other_text_h);
            int other_pill_h = other_text_h + (notif_padding_y * 2);
            stack_offset += other_pill_h + notif_stack_gap;
        }
        
        int x = base_x;
        uint32_t elapsed = now - n->start_time;
        if (elapsed < NOTIF_SLIDE_IN_MS) {
            int start_x = -pill_w;
            x = start_x + (int)((elapsed * (uint32_t)(base_x - start_x)) / NOTIF_SLIDE_IN_MS);
        }

        int y = base_y - pill_h - stack_offset;
        Uint8 alpha = notification_alpha(n, now);
        
        render_notification_pill(n, x, y, alpha, text_color, bg_color_sdl);
    }
}

void Notification_renderToLayer(int layer) {
    (void)layer; // unused now, kept for API compatibility
    
    if (!initialized) {
        PLAT_clearNotificationSurface();
        return;
    }
    
    int has_notifications = notification_count > 0;
    int has_system_indicator = system_indicator_type != SYSTEM_INDICATOR_NONE;
    int has_progress_indicator = progress_state.active;
    
    if (!has_notifications && !has_system_indicator && !has_progress_indicator) {
        // When all notifications and indicators are gone, render one final transparent frame
        if (gl_notification_surface) {
            if (needs_clear_frame) {
                SDL_FillRect(gl_notification_surface, NULL, 0);
                PLAT_setNotificationSurface(gl_notification_surface, 0, 0);
                needs_clear_frame = 0;
                render_dirty = 0;
                system_indicator_dirty = 0;
                progress_state.dirty = 0;
                last_system_indicator_type = SYSTEM_INDICATOR_NONE;
                return;
            }
            PLAT_clearNotificationSurface();
            SDL_FreeSurface(gl_notification_surface);
            gl_notification_surface = NULL;
        }
        return;
    }
    
    // We have notifications or indicators
    needs_clear_frame = 1;

    uint32_t now = SDL_GetTicks();
    int notifications_animating = 0;
    if (has_notifications) {
        for (int i = 0; i < notification_count; i++) {
            if (notification_is_animating(&notifications[i], now)) {
                notifications_animating = 1;
                break;
            }
        }
    }
    
    // Check if anything changed
    int notifications_changed = render_dirty || notification_count != last_notification_count || notifications_animating;
    int indicator_changed = system_indicator_dirty || system_indicator_type != last_system_indicator_type;
    int progress_changed = progress_state.dirty;
    
    if (!notifications_changed && !indicator_changed && !progress_changed) {
        return;
    }
    
    // Create surface if needed
    if (!gl_notification_surface) {
        gl_notification_surface = SDL_CreateRGBSurfaceWithFormat(
            0, screen_width, screen_height, 32, SDL_PIXELFORMAT_ABGR8888
        );
        if (!gl_notification_surface) {
            return;
        }
    }
    
    // Clear to transparent
    SDL_FillRect(gl_notification_surface, NULL, 0);
    
    // Render each element type
    if (has_system_indicator) {
        render_system_indicator();
    }
    if (has_progress_indicator) {
        render_progress_indicator();
    }
    if (has_notifications) {
        render_notification_stack(now);
    }
    
    // Set the notification surface for GL rendering
    PLAT_setNotificationSurface(gl_notification_surface, 0, 0);
    
    render_dirty = 0;
    last_notification_count = notification_count;
    system_indicator_dirty = 0;
    progress_state.dirty = 0;
    last_system_indicator_type = system_indicator_type;
}

bool Notification_isActive(void) {
    return initialized && notification_count > 0;
}

void Notification_clear(void) {
    notification_count = 0;
    progress_state.active = 0;
    progress_state.icon = NULL;
    render_dirty = 1;
    progress_state.dirty = 1;
    PLAT_clearNotificationSurface();
    if (gl_notification_surface) {
        SDL_FreeSurface(gl_notification_surface);
        gl_notification_surface = NULL;
    }
}

void Notification_quit(void) {
    Notification_clear();
    system_indicator_type = SYSTEM_INDICATOR_NONE;
    progress_state.active = 0;
    initialized = 0;
}

///////////////////////////////
// System Indicator Functions
///////////////////////////////

void Notification_showSystemIndicator(SystemIndicatorType type) {
    if (!initialized) return;
    if (type == SYSTEM_INDICATOR_NONE) return;
    
    // Update or start the indicator
    system_indicator_type = type;
    system_indicator_start_time = SDL_GetTicks();
    system_indicator_dirty = 1;
}

bool Notification_hasSystemIndicator(void) {
    return initialized && system_indicator_type != SYSTEM_INDICATOR_NONE;
}

int Notification_getSystemIndicatorWidth(void) {
    if (!initialized || system_indicator_type == SYSTEM_INDICATOR_NONE) {
        return 0;
    }
    return SCALE1(PILL_SIZE + SETTINGS_WIDTH + PADDING + SYS_INDICATOR_EXTRA_PAD);
}

///////////////////////////////
// Progress Indicator Functions
///////////////////////////////

void Notification_showProgressIndicator(const char* title, const char* progress, SDL_Surface* icon) {
    if (!initialized) return;
    
    // Check if RA notifications are enabled
    if (!CFG_getRAShowNotifications()) return;
    
    // Copy the title and progress strings
    strncpy(progress_state.title, title, PROGRESS_TITLE_MAX - 1);
    progress_state.title[PROGRESS_TITLE_MAX - 1] = '\0';
    
    strncpy(progress_state.progress, progress, PROGRESS_STRING_MAX - 1);
    progress_state.progress[PROGRESS_STRING_MAX - 1] = '\0';
    
    // Store icon reference (caller retains ownership)
    progress_state.icon = icon;
    
    // Activate and reset timer
    progress_state.active = 1;
    progress_state.start_time = SDL_GetTicks();
    progress_state.dirty = 1;
}

void Notification_hideProgressIndicator(void) {
    if (!initialized) return;
    
    if (progress_state.active) {
        progress_state.active = 0;
        progress_state.persistent = 0;
        progress_state.icon = NULL;
        progress_state.dirty = 1;
    }
}

void Notification_setProgressIndicatorPersistent(bool persistent) {
    progress_state.persistent = persistent ? 1 : 0;
}

bool Notification_hasProgressIndicator(void) {
    return initialized && progress_state.active;
}

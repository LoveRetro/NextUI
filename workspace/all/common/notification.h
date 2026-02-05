#ifndef __NOTIFICATION_H__
#define __NOTIFICATION_H__

#include "sdl.h"
#include <stdbool.h>
#include <stdint.h>

///////////////////////////////
// Notification System
// Toast-style notifications for save states, achievements, etc.
// Also handles system indicators (volume/brightness/colortemp) during gameplay.
///////////////////////////////

#define NOTIFICATION_MAX_QUEUE 4
#define NOTIFICATION_MAX_MESSAGE 64

// Duration for system indicators (in ms) - matches SETTING_DELAY
#define SYSTEM_INDICATOR_DURATION_MS 500

typedef enum {
    NOTIFICATION_SAVE_STATE,
    NOTIFICATION_LOAD_STATE,
    NOTIFICATION_SETTING,       // volume/brightness/colortemp adjustments
    NOTIFICATION_ACHIEVEMENT,   // RetroAchievements unlocks
} NotificationType;

typedef enum {
    NOTIFICATION_STATE_VISIBLE,   // Fully visible, waiting
    NOTIFICATION_STATE_DONE,      // Ready for removal
} NotificationState;

// System indicator types (volume/brightness/colortemp)
// These values match the show_setting values from PWR_update: 1=brightness, 2=volume, 3=colortemp
typedef enum {
    SYSTEM_INDICATOR_NONE = 0,
    SYSTEM_INDICATOR_BRIGHTNESS = 1,
    SYSTEM_INDICATOR_VOLUME = 2,
    SYSTEM_INDICATOR_COLORTEMP = 3,
} SystemIndicatorType;

typedef struct {
    NotificationType type;
    char message[NOTIFICATION_MAX_MESSAGE];
    SDL_Surface* icon;          // Optional, NULL for text-only (future use)
    uint32_t start_time;        // SDL_GetTicks() when notification started
    uint32_t duration_ms;       // How long to stay visible
    NotificationState state;
} Notification;

/**
 * Initialize the notification system.
 * Call once at startup after GFX is initialized.
 */
void Notification_init(void);

/**
 * Push a new notification to the queue.
 * @param type The notification type
 * @param message The message to display (copied internally)
 * @param icon Optional icon surface (can be NULL). Caller retains ownership.
 */
void Notification_push(NotificationType type, const char* message, SDL_Surface* icon);

/**
 * Update notification timeouts.
 * Call every frame with current tick count.
 * @param now Current SDL_GetTicks() value
 */
void Notification_update(uint32_t now);

/**
 * Render all active notifications to a specific layer.
 * Use this for OpenGL/layer-based rendering during gameplay.
 * @param layer The layer number (1-5, 5 being topmost)
 */
void Notification_renderToLayer(int layer);

/**
 * Check if there are any active notifications.
 * @return true if notifications are being displayed
 */
bool Notification_isActive(void);

/**
 * Clear all notifications immediately.
 */
void Notification_clear(void);

/**
 * Cleanup the notification system.
 * Call at shutdown.
 */
void Notification_quit(void);

///////////////////////////////
// System Indicators (Volume/Brightness/Colortemp)
// Always displayed in top-right, matches GFX_blitHardwareGroup visual style.
///////////////////////////////

/**
 * Show a system indicator (volume/brightness/colortemp).
 * System indicators are always displayed in the top-right corner.
 * They update in-place (no stacking) and have a short duration (~500ms).
 * @param type The indicator type (SYSTEM_INDICATOR_BRIGHTNESS, VOLUME, or COLORTEMP)
 */
void Notification_showSystemIndicator(SystemIndicatorType type);

/**
 * Check if a system indicator is currently being displayed.
 * @return true if a system indicator is active
 */
bool Notification_hasSystemIndicator(void);

/**
 * Get the width of the system indicator pill.
 * Useful for calculating positions of other elements.
 * @return The width in pixels, or 0 if no indicator is active
 */
int Notification_getSystemIndicatorWidth(void);

///////////////////////////////
// Achievement Progress Indicator
// Shows progress updates for measured achievements (e.g., "50/100 coins").
// Displayed in top-left, updates in place, auto-hides after timeout.
// Duration is controlled by CFG_getRAProgressNotificationDuration()
///////////////////////////////

/**
 * Show or update the achievement progress indicator.
 * Progress indicators are displayed in the top-left corner.
 * They update in-place and auto-hide after a timeout.
 * @param title Achievement title (copied internally)
 * @param progress Progress string like "50/100" (copied internally)
 * @param icon Optional badge icon (can be NULL). Caller retains ownership.
 */
void Notification_showProgressIndicator(const char* title, const char* progress, SDL_Surface* icon);

/**
 * Hide the achievement progress indicator immediately.
 * Called when RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE fires.
 */
void Notification_hideProgressIndicator(void);

/**
 * Set the progress indicator to persistent mode.
 * When persistent, the indicator won't auto-hide after the timeout.
 * Call hideProgressIndicator to dismiss it.
 * @param persistent true to keep visible until explicitly hidden
 */
void Notification_setProgressIndicatorPersistent(bool persistent);

/**
 * Check if a progress indicator is currently being displayed.
 * @return true if a progress indicator is active
 */
bool Notification_hasProgressIndicator(void);

#endif // __NOTIFICATION_H__

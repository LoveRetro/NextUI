#ifndef __RA_BADGES_H__
#define __RA_BADGES_H__

#include "sdl.h"
#include <stdbool.h>
#include <stddef.h>

// Implements the badge download/caching that the integration guide leaves to the emulator.
// See: https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration#showing-the-game-placard

// Badge size for notifications (will be scaled)
#define RA_BADGE_SIZE 64
#define RA_BADGE_NOTIFY_SIZE 24  // Size for notification icons

// Cache directory path (under SDCARD_PATH)
#define RA_BADGE_CACHE_DIR SHARED_USERDATA_PATH "/.ra/badges"

// Badge state
typedef enum {
	RA_BADGE_STATE_UNKNOWN,     // Badge not yet requested
	RA_BADGE_STATE_DOWNLOADING, // Download in progress
	RA_BADGE_STATE_CACHED,      // Downloaded and cached locally
	RA_BADGE_STATE_FAILED,      // Download failed
} RA_BadgeState;

/**
 * Initialize the badge cache system.
 * Creates cache directory if needed.
 */
void RA_Badges_init(void);

/**
 * Shutdown the badge cache system.
 * Clears any loaded surfaces (but keeps cached files).
 */
void RA_Badges_quit(void);

/**
 * Clear the in-memory badge surface cache.
 * Called when unloading a game to free memory.
 * Does not delete the on-disk cache.
 */
void RA_Badges_clearMemory(void);

/**
 * Pre-download all badges for the current game's achievements.
 * Should be called after game load when achievement list is available.
 * Downloads happen asynchronously in background threads.
 * 
 * @param achievements Array of achievement badge names
 * @param count Number of achievements
 */
void RA_Badges_prefetch(const char** badge_names, size_t count);

/**
 * Pre-download a single badge asynchronously.
 * 
 * @param badge_name The badge name (e.g., "00234")
 * @param locked Whether to download the locked version
 */
void RA_Badges_prefetchOne(const char* badge_name, bool locked);

/**
 * Get a badge surface. Returns cached surface or NULL if not available.
 * Downloads the badge if not cached (returns NULL immediately, call again later).
 * 
 * @param badge_name The badge name (e.g., "00234")
 * @param locked Whether to get the locked version (_lock suffix)
 * @return SDL_Surface* The badge surface, or NULL if not yet cached
 *         Caller does NOT own the surface - do not free it.
 */
SDL_Surface* RA_Badges_get(const char* badge_name, bool locked);

/**
 * Get a badge surface scaled to notification size.
 * 
 * @param badge_name The badge name (e.g., "00234")
 * @param locked Whether to get the locked version
 * @return SDL_Surface* The scaled badge surface, or NULL if not available
 *         Caller does NOT own the surface - do not free it.
 */
SDL_Surface* RA_Badges_getNotificationSize(const char* badge_name, bool locked);

/**
 * Get the state of a badge (whether it's cached, downloading, etc.)
 * 
 * @param badge_name The badge name
 * @param locked Whether to check the locked version
 * @return RA_BadgeState The current state
 */
RA_BadgeState RA_Badges_getState(const char* badge_name, bool locked);

/**
 * Get the cache file path for a badge.
 * 
 * @param badge_name The badge name
 * @param locked Whether to get the locked version path
 * @param buffer Output buffer for the path
 * @param buffer_size Size of the output buffer
 */
void RA_Badges_getCachePath(const char* badge_name, bool locked, char* buffer, size_t buffer_size);

/**
 * Build the URL for a badge.
 * 
 * @param badge_name The badge name
 * @param locked Whether to get the locked version URL
 * @param buffer Output buffer for the URL
 * @param buffer_size Size of the output buffer
 */
void RA_Badges_getUrl(const char* badge_name, bool locked, char* buffer, size_t buffer_size);

#endif // __RA_BADGES_H__

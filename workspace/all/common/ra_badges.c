#include "ra_badges.h"
#include "http.h"
#include "defines.h"
#include "sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

// Logging macro
#define BADGE_LOG(fmt, ...) printf("[RA_BADGES] " fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Constants
 *****************************************************************************/

#define RA_BADGE_BASE_URL "https://media.retroachievements.org/Badge/"
#define MAX_BADGE_NAME 32
#define MAX_CACHED_BADGES 256

/*****************************************************************************
 * Badge cache entry
 *****************************************************************************/

typedef struct {
	char badge_name[MAX_BADGE_NAME];
	bool locked;
	RA_BadgeState state;
	SDL_Surface* surface;
	SDL_Surface* surface_scaled;  // Pre-scaled for notifications
} BadgeCacheEntry;

/*****************************************************************************
 * Static state
 *****************************************************************************/

static BadgeCacheEntry badge_cache[MAX_CACHED_BADGES];
static int badge_cache_count = 0;
static SDL_mutex* badge_mutex = NULL;
static int pending_downloads = 0;
static bool initialized = false;

/*****************************************************************************
 * Internal helpers
 *****************************************************************************/

// Find or create cache entry for a badge
static BadgeCacheEntry* find_or_create_entry(const char* badge_name, bool locked) {
	// Search existing entries
	for (int i = 0; i < badge_cache_count; i++) {
		if (badge_cache[i].locked == locked &&
		    strcmp(badge_cache[i].badge_name, badge_name) == 0) {
			return &badge_cache[i];
		}
	}
	
	// Create new entry if space available
	if (badge_cache_count >= MAX_CACHED_BADGES) {
		BADGE_LOG("Cache full, cannot add badge %s\n", badge_name);
		return NULL;
	}
	
	BadgeCacheEntry* entry = &badge_cache[badge_cache_count++];
	memset(entry, 0, sizeof(BadgeCacheEntry));
	strncpy(entry->badge_name, badge_name, MAX_BADGE_NAME - 1);
	entry->locked = locked;
	entry->state = RA_BADGE_STATE_UNKNOWN;
	
	return entry;
}

// Create cache directory if it doesn't exist
static void ensure_cache_dir(void) {
	char path[MAX_PATH];
	
	// Create .cache directory
	snprintf(path, sizeof(path), "%s/.cache", SDCARD_PATH);
	mkdir(path, 0755);
	
	// Create .cache/ra directory
	snprintf(path, sizeof(path), "%s/.cache/ra", SDCARD_PATH);
	mkdir(path, 0755);
	
	// Create .cache/ra/badges directory
	snprintf(path, sizeof(path), "%s%s", SDCARD_PATH, RA_BADGE_CACHE_DIR);
	mkdir(path, 0755);
}

// Check if cache file exists
static bool cache_file_exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && st.st_size > 0;
}

// Save HTTP response data to cache file
static bool save_to_cache(const char* path, const char* data, size_t size) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		BADGE_LOG("Failed to open cache file for writing: %s\n", path);
		return false;
	}
	
	size_t written = fwrite(data, 1, size, f);
	fclose(f);
	
	if (written != size) {
		BADGE_LOG("Failed to write cache file: %s\n", path);
		unlink(path);
		return false;
	}
	
	return true;
}

// Load badge image from cache
static SDL_Surface* load_from_cache(const char* path) {
	SDL_Surface* surface = IMG_Load(path);
	if (!surface) {
		BADGE_LOG("Failed to load badge image: %s - %s\n", path, IMG_GetError());
		return NULL;
	}
	return surface;
}

// Scale a surface to target size using SDL_BlitScaled for proper format handling
static SDL_Surface* scale_surface(SDL_Surface* src, int target_size) {
	if (!src) return NULL;
	
	// Calculate scale factor to fit in target_size x target_size
	float scale_x = (float)target_size / src->w;
	float scale_y = (float)target_size / src->h;
	float scale = (scale_x < scale_y) ? scale_x : scale_y;
	
	int new_w = (int)(src->w * scale);
	int new_h = (int)(src->h * scale);
	
	// Create scaled surface with alpha support
	SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(
		0, new_w, new_h, 32, SDL_PIXELFORMAT_RGBA32
	);
	if (!scaled) {
		return NULL;
	}
	
	// Clear to transparent
	SDL_FillRect(scaled, NULL, 0);
	
	// Use SDL_BlitScaled which handles pixel format conversion properly
	SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
	SDL_Rect dst_rect = {0, 0, new_w, new_h};
	SDL_BlitScaled(src, NULL, scaled, &dst_rect);
	
	return scaled;
}

/*****************************************************************************
 * Download callback
 *****************************************************************************/

typedef struct {
	char badge_name[MAX_BADGE_NAME];
	bool locked;
	char cache_path[MAX_PATH];
} DownloadContext;

static void badge_download_callback(HTTP_Response* response, void* userdata) {
	DownloadContext* ctx = (DownloadContext*)userdata;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	pending_downloads--;
	if (pending_downloads < 0) pending_downloads = 0;
	
	BadgeCacheEntry* entry = find_or_create_entry(ctx->badge_name, ctx->locked);
	
	if (response && response->data && response->http_status == 200 && !response->error) {
		// Save to cache
		if (save_to_cache(ctx->cache_path, response->data, response->size)) {
			// Load the image
			if (entry) {
				entry->surface = load_from_cache(ctx->cache_path);
				if (entry->surface) {
					entry->surface_scaled = scale_surface(entry->surface, RA_BADGE_NOTIFY_SIZE);
					entry->state = RA_BADGE_STATE_CACHED;
				} else {
					entry->state = RA_BADGE_STATE_FAILED;
				}
			}
		} else if (entry) {
			entry->state = RA_BADGE_STATE_FAILED;
		}
	} else {
		BADGE_LOG("Failed to download badge %s%s: %s\n",
		          ctx->badge_name, ctx->locked ? "_lock" : "",
		          response && response->error ? response->error : "HTTP error");
		if (entry) {
			entry->state = RA_BADGE_STATE_FAILED;
		}
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
	
	if (response) {
		HTTP_freeResponse(response);
	}
	free(ctx);
}

// Start downloading a badge
static void start_download(const char* badge_name, bool locked) {
	if (!initialized) return;
	
	BadgeCacheEntry* entry = find_or_create_entry(badge_name, locked);
	if (!entry) return;
	
	// Check if already downloading or cached
	if (entry->state == RA_BADGE_STATE_DOWNLOADING ||
	    entry->state == RA_BADGE_STATE_CACHED) {
		return;
	}
	
	// Build URL and cache path
	char url[512];
	char cache_path[MAX_PATH];
	RA_Badges_getUrl(badge_name, locked, url, sizeof(url));
	RA_Badges_getCachePath(badge_name, locked, cache_path, sizeof(cache_path));
	
	// Check if already cached on disk
	if (cache_file_exists(cache_path)) {
		entry->surface = load_from_cache(cache_path);
		if (entry->surface) {
			entry->surface_scaled = scale_surface(entry->surface, RA_BADGE_NOTIFY_SIZE);
			entry->state = RA_BADGE_STATE_CACHED;
			return;
		}
	}
	
	// Need to download
	DownloadContext* ctx = (DownloadContext*)malloc(sizeof(DownloadContext));
	if (!ctx) return;
	
	strncpy(ctx->badge_name, badge_name, MAX_BADGE_NAME - 1);
	ctx->badge_name[MAX_BADGE_NAME - 1] = '\0';
	ctx->locked = locked;
	strncpy(ctx->cache_path, cache_path, MAX_PATH - 1);
	ctx->cache_path[MAX_PATH - 1] = '\0';
	
	entry->state = RA_BADGE_STATE_DOWNLOADING;
	pending_downloads++;
	
	HTTP_getAsync(url, badge_download_callback, ctx);
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

void RA_Badges_init(void) {
	if (initialized) return;
	
	badge_mutex = SDL_CreateMutex();
	badge_cache_count = 0;
	pending_downloads = 0;
	memset(badge_cache, 0, sizeof(badge_cache));
	
	ensure_cache_dir();
	
	initialized = true;
}

void RA_Badges_quit(void) {
	if (!initialized) return;
	
	RA_Badges_clearMemory();
	
	if (badge_mutex) {
		SDL_DestroyMutex(badge_mutex);
		badge_mutex = NULL;
	}
	
	initialized = false;
}

void RA_Badges_clearMemory(void) {
	if (!initialized) return;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	for (int i = 0; i < badge_cache_count; i++) {
		if (badge_cache[i].surface) {
			SDL_FreeSurface(badge_cache[i].surface);
			badge_cache[i].surface = NULL;
		}
		if (badge_cache[i].surface_scaled) {
			SDL_FreeSurface(badge_cache[i].surface_scaled);
			badge_cache[i].surface_scaled = NULL;
		}
	}
	badge_cache_count = 0;
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
}

void RA_Badges_prefetch(const char** badge_names, size_t count) {
	if (!initialized) return;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	for (size_t i = 0; i < count; i++) {
		if (badge_names[i] && badge_names[i][0]) {
			// Download both locked and unlocked versions
			start_download(badge_names[i], false);
			start_download(badge_names[i], true);
		}
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
}

void RA_Badges_prefetchOne(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	start_download(badge_name, locked);
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
}

SDL_Surface* RA_Badges_get(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return NULL;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	BadgeCacheEntry* entry = find_or_create_entry(badge_name, locked);
	SDL_Surface* result = NULL;
	
	if (entry) {
		if (entry->state == RA_BADGE_STATE_CACHED && entry->surface) {
			result = entry->surface;
		} else if (entry->state == RA_BADGE_STATE_UNKNOWN) {
			// Trigger download
			start_download(badge_name, locked);
		}
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
	
	return result;
}

SDL_Surface* RA_Badges_getNotificationSize(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return NULL;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	BadgeCacheEntry* entry = find_or_create_entry(badge_name, locked);
	SDL_Surface* result = NULL;
	
	if (entry) {
		if (entry->state == RA_BADGE_STATE_CACHED && entry->surface_scaled) {
			result = entry->surface_scaled;
		} else if (entry->state == RA_BADGE_STATE_UNKNOWN) {
			// Trigger download
			start_download(badge_name, locked);
		}
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
	
	return result;
}

RA_BadgeState RA_Badges_getState(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return RA_BADGE_STATE_UNKNOWN;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	RA_BadgeState state = RA_BADGE_STATE_UNKNOWN;
	
	for (int i = 0; i < badge_cache_count; i++) {
		if (badge_cache[i].locked == locked &&
		    strcmp(badge_cache[i].badge_name, badge_name) == 0) {
			state = badge_cache[i].state;
			break;
		}
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
	
	return state;
}

bool RA_Badges_hasPendingDownloads(void) {
	if (!initialized) return false;
	return pending_downloads > 0;
}

void RA_Badges_getCachePath(const char* badge_name, bool locked, char* buffer, size_t buffer_size) {
	if (locked) {
		snprintf(buffer, buffer_size, "%s%s/%s_lock.png", 
		         SDCARD_PATH, RA_BADGE_CACHE_DIR, badge_name);
	} else {
		snprintf(buffer, buffer_size, "%s%s/%s.png",
		         SDCARD_PATH, RA_BADGE_CACHE_DIR, badge_name);
	}
}

void RA_Badges_getUrl(const char* badge_name, bool locked, char* buffer, size_t buffer_size) {
	if (locked) {
		snprintf(buffer, buffer_size, "%s%s_lock.png", RA_BADGE_BASE_URL, badge_name);
	} else {
		snprintf(buffer, buffer_size, "%s%s.png", RA_BADGE_BASE_URL, badge_name);
	}
}

#include "ra_badges.h"
#include "http.h"
#include "defines.h"
#include "api.h"
#include "sdl.h"
#include "notification.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

// Logging macros using NextUI's LOG_* infrastructure
#define BADGE_LOG_DEBUG(fmt, ...) LOG_debug("[RA_BADGES] " fmt, ##__VA_ARGS__)
#define BADGE_LOG_INFO(fmt, ...)  LOG_info("[RA_BADGES] " fmt, ##__VA_ARGS__)
#define BADGE_LOG_WARN(fmt, ...)  LOG_warn("[RA_BADGES] " fmt, ##__VA_ARGS__)
#define BADGE_LOG_ERROR(fmt, ...) LOG_error("[RA_BADGES] " fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Constants
 *****************************************************************************/

#define RA_BADGE_BASE_URL "https://media.retroachievements.org/Badge/"
#define MAX_BADGE_NAME 32
#define MAX_CACHED_BADGES 256
#define MAX_CONCURRENT_DOWNLOADS 8
#define MAX_QUEUED_DOWNLOADS 512

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
 * Download queue entry
 *****************************************************************************/

typedef struct {
	char badge_name[MAX_BADGE_NAME];
	bool locked;
} QueuedDownload;

/*****************************************************************************
 * Static state
 *****************************************************************************/

static BadgeCacheEntry badge_cache[MAX_CACHED_BADGES];
static int badge_cache_count = 0;
static SDL_mutex* badge_mutex = NULL;
static int pending_downloads = 0;
static bool initialized = false;

// Download queue for rate limiting
typedef struct {
	QueuedDownload items[MAX_QUEUED_DOWNLOADS];
	int head;
	int tail;
	int count;
	int active;
} DownloadQueueState;

static DownloadQueueState download_queue = {0};

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
		BADGE_LOG_WARN("Cache full, cannot add badge %s\n", badge_name);
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
	
	// Create .ra directory
	snprintf(path, sizeof(path), SHARED_USERDATA_PATH "/.ra");
	mkdir(path, 0755);
	
	// Create .ra/badges directory
	mkdir(RA_BADGE_CACHE_DIR, 0755);
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
		BADGE_LOG_ERROR("Failed to open cache file for writing: %s\n", path);
		return false;
	}
	
	size_t written = fwrite(data, 1, size, f);
	fclose(f);
	
	if (written != size) {
		BADGE_LOG_ERROR("Failed to write cache file: %s\n", path);
		unlink(path);
		return false;
	}
	
	return true;
}

// Load badge image from cache
static SDL_Surface* load_from_cache(const char* path) {
	SDL_Surface* surface = IMG_Load(path);
	if (!surface) {
		BADGE_LOG_WARN("Failed to load badge image: %s - %s\n", path, IMG_GetError());
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

// Forward declarations
static void process_download_queue(void);
static void badge_download_callback(HTTP_Response* response, void* userdata);

// Queue a download for later processing (must hold mutex)
static void queue_download(const char* badge_name, bool locked) {
	if (download_queue.count >= MAX_QUEUED_DOWNLOADS) {
		BADGE_LOG_WARN("Download queue full, dropping badge %s\n", badge_name);
		return;
	}
	
	QueuedDownload* item = &download_queue.items[download_queue.tail];
	strncpy(item->badge_name, badge_name, MAX_BADGE_NAME - 1);
	item->badge_name[MAX_BADGE_NAME - 1] = '\0';
	item->locked = locked;
	
	download_queue.tail = (download_queue.tail + 1) % MAX_QUEUED_DOWNLOADS;
	download_queue.count++;
}

// Dequeue and start a download (must hold mutex)
static bool dequeue_and_start_download(void) {
	if (download_queue.count == 0) return false;
	
	QueuedDownload* item = &download_queue.items[download_queue.head];
	download_queue.head = (download_queue.head + 1) % MAX_QUEUED_DOWNLOADS;
	download_queue.count--;
	
	// Get entry and check if still needs download
	BadgeCacheEntry* entry = find_or_create_entry(item->badge_name, item->locked);
	if (!entry) return false;
	
	// Skip if already cached (might have been cached while queued)
	if (entry->state == RA_BADGE_STATE_CACHED) {
		return false;
	}
	
	// Build URL and cache path
	char url[512];
	char cache_path[MAX_PATH];
	RA_Badges_getUrl(item->badge_name, item->locked, url, sizeof(url));
	RA_Badges_getCachePath(item->badge_name, item->locked, cache_path, sizeof(cache_path));
	
	// Check if already cached on disk
	if (cache_file_exists(cache_path)) {
		entry->state = RA_BADGE_STATE_CACHED;
		return false;
	}
	
	// Start download
	DownloadContext* ctx = (DownloadContext*)malloc(sizeof(DownloadContext));
	if (!ctx) return false;
	
	strncpy(ctx->badge_name, item->badge_name, MAX_BADGE_NAME - 1);
	ctx->badge_name[MAX_BADGE_NAME - 1] = '\0';
	ctx->locked = item->locked;
	strncpy(ctx->cache_path, cache_path, MAX_PATH - 1);
	ctx->cache_path[MAX_PATH - 1] = '\0';
	
	entry->state = RA_BADGE_STATE_DOWNLOADING;
	download_queue.active++;
	pending_downloads++;
	
	HTTP_getAsync(url, badge_download_callback, ctx);
	return true;
}

// Process queued downloads up to the concurrency limit (must hold mutex)
static void process_download_queue(void) {
	while (download_queue.active < MAX_CONCURRENT_DOWNLOADS && download_queue.count > 0) {
		if (!dequeue_and_start_download()) {
			// Item was skipped (already cached), try next
			continue;
		}
	}
}

static void badge_download_callback(HTTP_Response* response, void* userdata) {
	DownloadContext* ctx = (DownloadContext*)userdata;
	
	bool success = false;
	
	// Just save to disk - don't load into memory during prefetch
	// Images will be loaded lazily when actually needed for display
	if (response && response->data && response->http_status == 200 && !response->error) {
		success = save_to_cache(ctx->cache_path, response->data, response->size);
		if (!success) {
			BADGE_LOG_WARN("Failed to save badge %s%s to cache\n",
			          ctx->badge_name, ctx->locked ? "_lock" : "");
		}
	} else {
		BADGE_LOG_WARN("Failed to download badge %s%s: %s\n",
		          ctx->badge_name, ctx->locked ? "_lock" : "",
		          response && response->error ? response->error : "HTTP error");
	}
	
	// Only hold mutex briefly to update state
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	download_queue.active--;
	if (download_queue.active < 0) download_queue.active = 0;
	
	pending_downloads--;
	if (pending_downloads < 0) pending_downloads = 0;
	
	BadgeCacheEntry* entry = find_or_create_entry(ctx->badge_name, ctx->locked);
	if (entry) {
		// Mark as cached (on disk) - surfaces will be loaded lazily
		entry->state = success ? RA_BADGE_STATE_CACHED : RA_BADGE_STATE_FAILED;
	}
	
	// Start next queued download(s)
	process_download_queue();
	
	// Hide progress indicator when all downloads complete
	if (pending_downloads == 0 && download_queue.count == 0) {
		Notification_hideProgressIndicator();
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
	
	if (response) {
		HTTP_freeResponse(response);
	}
	free(ctx);
}

// Request a badge download - queues if at concurrency limit
static void start_download(const char* badge_name, bool locked) {
	if (!initialized) return;
	
	BadgeCacheEntry* entry = find_or_create_entry(badge_name, locked);
	if (!entry) return;
	
	// Check if already downloading or cached
	if (entry->state == RA_BADGE_STATE_DOWNLOADING ||
	    entry->state == RA_BADGE_STATE_CACHED) {
		return;
	}
	
	// Check if already cached on disk
	char cache_path[MAX_PATH];
	RA_Badges_getCachePath(badge_name, locked, cache_path, sizeof(cache_path));
	if (cache_file_exists(cache_path)) {
		entry->state = RA_BADGE_STATE_CACHED;
		return;
	}
	
	// Queue the download - state will be set when download actually starts
	queue_download(badge_name, locked);
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

void RA_Badges_init(void) {
	if (initialized) return;
	
	badge_mutex = SDL_CreateMutex();
	badge_cache_count = 0;
	pending_downloads = 0;
	download_queue.head = 0;
	download_queue.tail = 0;
	download_queue.count = 0;
	download_queue.active = 0;
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
			// Queue both locked and unlocked versions
			start_download(badge_names[i], false);
			start_download(badge_names[i], true);
		}
	}
	
	// Show progress indicator if downloads were queued
	if (download_queue.count > 0) {
		Notification_setProgressIndicatorPersistent(true);
		Notification_showProgressIndicator("Loading achievement badges...", "", NULL);
		
		// Start processing the queue (up to MAX_CONCURRENT_DOWNLOADS)
		process_download_queue();
	}
	
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
}

void RA_Badges_prefetchOne(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	start_download(badge_name, locked);
	process_download_queue();
	if (badge_mutex) SDL_UnlockMutex(badge_mutex);
}

SDL_Surface* RA_Badges_get(const char* badge_name, bool locked) {
	if (!initialized || !badge_name || !badge_name[0]) return NULL;
	
	if (badge_mutex) SDL_LockMutex(badge_mutex);
	
	BadgeCacheEntry* entry = find_or_create_entry(badge_name, locked);
	SDL_Surface* result = NULL;
	
	if (entry) {
		if (entry->state == RA_BADGE_STATE_CACHED) {
			// Lazy load from disk if not in memory
			if (!entry->surface) {
				char cache_path[MAX_PATH];
				RA_Badges_getCachePath(badge_name, locked, cache_path, sizeof(cache_path));
				entry->surface = load_from_cache(cache_path);
				if (entry->surface) {
					entry->surface_scaled = scale_surface(entry->surface, RA_BADGE_NOTIFY_SIZE);
				}
			}
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
		if (entry->state == RA_BADGE_STATE_CACHED) {
			// Lazy load from disk if not in memory
			if (!entry->surface_scaled) {
				char cache_path[MAX_PATH];
				RA_Badges_getCachePath(badge_name, locked, cache_path, sizeof(cache_path));
				entry->surface = load_from_cache(cache_path);
				if (entry->surface) {
					entry->surface_scaled = scale_surface(entry->surface, RA_BADGE_NOTIFY_SIZE);
				}
			}
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

void RA_Badges_getCachePath(const char* badge_name, bool locked, char* buffer, size_t buffer_size) {
	if (locked) {
		snprintf(buffer, buffer_size, "%s/%s_lock.png", 
		         RA_BADGE_CACHE_DIR, badge_name);
	} else {
		snprintf(buffer, buffer_size, "%s/%s.png",
		         RA_BADGE_CACHE_DIR, badge_name);
	}
}

void RA_Badges_getUrl(const char* badge_name, bool locked, char* buffer, size_t buffer_size) {
	if (locked) {
		snprintf(buffer, buffer_size, "%s%s_lock.png", RA_BADGE_BASE_URL, badge_name);
	} else {
		snprintf(buffer, buffer_size, "%s%s.png", RA_BADGE_BASE_URL, badge_name);
	}
}

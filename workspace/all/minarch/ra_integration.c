#include "ra_integration.h"
#include "ra_consoles.h"
#include "rc_client.h"
#include "rc_libretro.h"
#include "rc_hash.h"
#include "chd_reader.h"
#include "config.h"
#include "http.h"
#include "notification.h"
#include "ra_badges.h"
#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <SDL2/SDL.h>

// Logging macro
#define RA_LOG(fmt, ...) printf("[RA] " fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Static state
 *****************************************************************************/

static rc_client_t* ra_client = NULL;
static bool ra_game_loaded = false;
static bool ra_logged_in = false;

// Current game hash (for mute file path)
static char ra_game_hash[64] = {0};

// Muted achievements tracking
#define RA_MAX_MUTED_ACHIEVEMENTS 1024
static uint32_t ra_muted_achievements[RA_MAX_MUTED_ACHIEVEMENTS];
static int ra_muted_count = 0;
static bool ra_muted_dirty = false;  // Track if mute state needs saving

// Memory access function pointers (set by minarch)
static RA_GetMemoryFunc ra_get_memory_data = NULL;
static RA_GetMemorySizeFunc ra_get_memory_size = NULL;

// Memory map from core (via RETRO_ENVIRONMENT_SET_MEMORY_MAPS)
// We store a deep copy because the core's data may be on the stack or freed
static struct retro_memory_map* ra_memory_map = NULL;
static struct retro_memory_descriptor* ra_memory_map_descriptors = NULL;

// Memory regions for rcheevos (initialized per-game based on console type)
static rc_libretro_memory_regions_t ra_memory_regions;
static bool ra_memory_regions_initialized = false;

// Pending game load storage (for async login race condition)
#define RA_MAX_PATH 512
static char pending_rom_path[RA_MAX_PATH];
static uint8_t* pending_rom_data = NULL;
static size_t pending_rom_size = 0;
static char pending_emu_tag[16];
static bool pending_game_load = false;

// Login retry state
static int ra_login_retry_count = 0;
#define RA_LOGIN_MAX_RETRIES 5
static uint32_t ra_login_retry_time = 0;      // SDL_GetTicks() timestamp for next retry
static bool ra_login_retry_pending = false;
static bool ra_login_notified_connecting = false;  // Track if we showed "Connecting..." notification

// Wifi wait config
#define RA_WIFI_WAIT_MAX_MS 3000   // 3 seconds max blocking wait
#define RA_WIFI_WAIT_POLL_MS 500   // Check every 500ms

/*****************************************************************************
 * Thread-safe response queue
 * 
 * HTTP callbacks are invoked from worker threads, but rcheevos callbacks
 * and our integration code access shared state that isn't thread-safe.
 * We queue HTTP responses and process them on the main thread in RA_idle().
 *****************************************************************************/

typedef struct {
	char* body;                           // Owned copy of response body
	size_t body_length;
	int http_status_code;
	rc_client_server_callback_t callback;
	void* callback_data;
} RA_QueuedResponse;

#define RA_RESPONSE_QUEUE_SIZE 16
static RA_QueuedResponse ra_response_queue[RA_RESPONSE_QUEUE_SIZE];
static volatile int ra_response_queue_count = 0;
static SDL_mutex* ra_queue_mutex = NULL;

// Forward declarations for queue functions
static void ra_queue_init(void);
static void ra_queue_quit(void);
static bool ra_queue_push(const char* body, size_t body_length, int http_status,
                          rc_client_server_callback_t callback, void* callback_data);
static bool ra_queue_pop(RA_QueuedResponse* out);
static void ra_process_queued_responses(void);

// Forward declarations for helper functions
static void ra_clear_pending_game(void);
static void ra_do_load_game(const char* rom_path, const uint8_t* rom_data, size_t rom_size, const char* emu_tag);
static void ra_load_muted_achievements(void);
static void ra_save_muted_achievements(void);
static void ra_clear_muted_achievements(void);
static void ra_reset_login_state(void);
static void ra_start_login(void);
static uint32_t ra_get_retry_delay_ms(int attempt);
static void ra_login_callback(int result, const char* error_message, rc_client_t* client, void* userdata);

/*****************************************************************************
 * CHD (compressed hunks of data) reader support for disc images
 * 
 * The default rcheevos CD reader only supports CUE/BIN and ISO formats.
 * We wrap the CD reader callbacks to try CHD first, then fall back to default.
 * 
 * We use a wrapper handle to track whether a handle came from CHD or default
 * reader, so we can route subsequent calls to the correct implementation.
 *****************************************************************************/

// Store default CD reader callbacks for fallback
static rc_hash_cdreader_t ra_default_cdreader;

// Wrapper handle to distinguish CHD vs default reader handles
#define RA_CDHANDLE_MAGIC 0x43484448  // "CHDH"
typedef struct {
	uint32_t magic;      // Magic number to identify our wrapper
	bool is_chd;         // true = CHD handle, false = default reader handle
	void* inner_handle;  // The actual handle from CHD or default reader
} ra_cdreader_handle_t;

// Helper to create a wrapper handle
static void* ra_cdreader_wrap_handle(void* inner_handle, bool is_chd) {
	if (!inner_handle) return NULL;
	
	ra_cdreader_handle_t* wrapper = (ra_cdreader_handle_t*)malloc(sizeof(ra_cdreader_handle_t));
	if (!wrapper) {
		// Failed to allocate wrapper - close the inner handle
		if (is_chd) {
			chd_close_track(inner_handle);
		} else if (ra_default_cdreader.close_track) {
			ra_default_cdreader.close_track(inner_handle);
		}
		return NULL;
	}
	
	wrapper->magic = RA_CDHANDLE_MAGIC;
	wrapper->is_chd = is_chd;
	wrapper->inner_handle = inner_handle;
	return wrapper;
}

// Helper to validate and unwrap handle
static ra_cdreader_handle_t* ra_cdreader_unwrap(void* handle) {
	if (!handle) return NULL;
	ra_cdreader_handle_t* wrapper = (ra_cdreader_handle_t*)handle;
	if (wrapper->magic != RA_CDHANDLE_MAGIC) return NULL;
	return wrapper;
}

// Wrapper: Try CHD first, then default
static void* ra_cdreader_open_track(const char* path, uint32_t track) {
	// Try CHD reader first
	void* handle = chd_open_track(path, track);
	if (handle) {
		return ra_cdreader_wrap_handle(handle, true);
	}
	// Fall back to default reader
	if (ra_default_cdreader.open_track) {
		handle = ra_default_cdreader.open_track(path, track);
		if (handle) {
			return ra_cdreader_wrap_handle(handle, false);
		}
	}
	return NULL;
}

static void* ra_cdreader_open_track_iterator(const char* path, uint32_t track, const rc_hash_iterator_t* iterator) {
	// Try CHD reader first
	void* handle = chd_open_track_iterator(path, track, iterator);
	if (handle) {
		return ra_cdreader_wrap_handle(handle, true);
	}
	// Fall back to default reader
	if (ra_default_cdreader.open_track_iterator) {
		handle = ra_default_cdreader.open_track_iterator(path, track, iterator);
		if (handle) {
			return ra_cdreader_wrap_handle(handle, false);
		}
	}
	if (ra_default_cdreader.open_track) {
		handle = ra_default_cdreader.open_track(path, track);
		if (handle) {
			return ra_cdreader_wrap_handle(handle, false);
		}
	}
	return NULL;
}

static size_t ra_cdreader_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
	ra_cdreader_handle_t* wrapper = ra_cdreader_unwrap(track_handle);
	if (!wrapper) return 0;
	
	if (wrapper->is_chd) {
		return chd_read_sector(wrapper->inner_handle, sector, buffer, requested_bytes);
	} else if (ra_default_cdreader.read_sector) {
		return ra_default_cdreader.read_sector(wrapper->inner_handle, sector, buffer, requested_bytes);
	}
	return 0;
}

static void ra_cdreader_close_track(void* track_handle) {
	ra_cdreader_handle_t* wrapper = ra_cdreader_unwrap(track_handle);
	if (!wrapper) return;
	
	if (wrapper->is_chd) {
		chd_close_track(wrapper->inner_handle);
	} else if (ra_default_cdreader.close_track) {
		ra_default_cdreader.close_track(wrapper->inner_handle);
	}
	
	// Clear magic and free wrapper
	wrapper->magic = 0;
	free(wrapper);
}

static uint32_t ra_cdreader_first_track_sector(void* track_handle) {
	ra_cdreader_handle_t* wrapper = ra_cdreader_unwrap(track_handle);
	if (!wrapper) return 0;
	
	if (wrapper->is_chd) {
		return chd_first_track_sector(wrapper->inner_handle);
	} else if (ra_default_cdreader.first_track_sector) {
		return ra_default_cdreader.first_track_sector(wrapper->inner_handle);
	}
	return 0;
}

// Initialize CHD-aware CD reader callbacks
static void ra_init_cdreader(void) {
	// Get default callbacks to use as fallback
	rc_hash_get_default_cdreader(&ra_default_cdreader);
	
	RA_LOG("Initializing CHD-aware CD reader\n");
}

/*****************************************************************************
 * Helper: Get retry delay for login attempts
 *****************************************************************************/
static uint32_t ra_get_retry_delay_ms(int attempt) {
	// Delays: 1s, 2s, 4s, 8s, 8s
	uint32_t delays[] = {1000, 2000, 4000, 8000, 8000};
	int idx = (attempt < 5) ? attempt : 4;
	return delays[idx];
}

/*****************************************************************************
 * Helper: Reset login retry state
 *****************************************************************************/
static void ra_reset_login_state(void) {
	ra_login_retry_count = 0;
	ra_login_retry_pending = false;
	ra_login_retry_time = 0;
	ra_login_notified_connecting = false;
}

/*****************************************************************************
 * Helper: Start a login attempt
 *****************************************************************************/
static void ra_start_login(void) {
	RA_LOG("Attempting login (attempt %d/%d)...\n", 
	       ra_login_retry_count + 1, RA_LOGIN_MAX_RETRIES);
	rc_client_begin_login_with_token(ra_client,
		CFG_getRAUsername(), CFG_getRAToken(),
		ra_login_callback, NULL);
}

/*****************************************************************************
 * Response queue implementation
 * 
 * Thread-safe circular queue for HTTP responses. Worker threads push,
 * main thread pops and processes in RA_idle().
 *****************************************************************************/

static void ra_queue_init(void) {
	if (!ra_queue_mutex) {
		ra_queue_mutex = SDL_CreateMutex();
	}
	ra_response_queue_count = 0;
	memset(ra_response_queue, 0, sizeof(ra_response_queue));
}

static void ra_queue_quit(void) {
	// Drain any pending responses
	if (ra_queue_mutex) {
		SDL_LockMutex(ra_queue_mutex);
		for (int i = 0; i < ra_response_queue_count; i++) {
			free(ra_response_queue[i].body);
			ra_response_queue[i].body = NULL;
		}
		ra_response_queue_count = 0;
		SDL_UnlockMutex(ra_queue_mutex);
		
		SDL_DestroyMutex(ra_queue_mutex);
		ra_queue_mutex = NULL;
	}
}

// Called from worker thread - enqueue a response for main thread processing
static bool ra_queue_push(const char* body, size_t body_length, int http_status,
                          rc_client_server_callback_t callback, void* callback_data) {
	if (!ra_queue_mutex) {
		return false;
	}
	
	bool success = false;
	SDL_LockMutex(ra_queue_mutex);
	
	if (ra_response_queue_count < RA_RESPONSE_QUEUE_SIZE) {
		RA_QueuedResponse* resp = &ra_response_queue[ra_response_queue_count];
		
		// Copy the body data (caller will free original)
		if (body && body_length > 0) {
			resp->body = (char*)malloc(body_length + 1);
			if (resp->body) {
				memcpy(resp->body, body, body_length);
				resp->body[body_length] = '\0';
				resp->body_length = body_length;
			} else {
				resp->body_length = 0;
			}
		} else {
			resp->body = NULL;
			resp->body_length = 0;
		}
		
		resp->http_status_code = http_status;
		resp->callback = callback;
		resp->callback_data = callback_data;
		
		ra_response_queue_count++;
		success = true;
	} else {
		RA_LOG("Warning: Response queue full, dropping response\n");
	}
	
	SDL_UnlockMutex(ra_queue_mutex);
	return success;
}

// Called from main thread - dequeue a response for processing
static bool ra_queue_pop(RA_QueuedResponse* out) {
	if (!ra_queue_mutex || !out) {
		return false;
	}
	
	bool has_item = false;
	SDL_LockMutex(ra_queue_mutex);
	
	if (ra_response_queue_count > 0) {
		// Copy first item to output
		*out = ra_response_queue[0];
		
		// Shift remaining items down
		for (int i = 0; i < ra_response_queue_count - 1; i++) {
			ra_response_queue[i] = ra_response_queue[i + 1];
		}
		ra_response_queue_count--;
		
		// Clear the last slot
		memset(&ra_response_queue[ra_response_queue_count], 0, sizeof(RA_QueuedResponse));
		
		has_item = true;
	}
	
	SDL_UnlockMutex(ra_queue_mutex);
	return has_item;
}

// Called from main thread in RA_idle() - process all queued responses
static void ra_process_queued_responses(void) {
	RA_QueuedResponse resp;
	
	while (ra_queue_pop(&resp)) {
		// Build the server response structure
		rc_api_server_response_t server_response;
		memset(&server_response, 0, sizeof(server_response));
		
		server_response.body = resp.body;
		server_response.body_length = resp.body_length;
		server_response.http_status_code = resp.http_status_code;
		
		// Invoke the rcheevos callback on the main thread
		if (resp.callback) {
			resp.callback(&server_response, resp.callback_data);
		}
		
		// Free our copy of the body
		free(resp.body);
	}
}

/*****************************************************************************
 * Helper: Muted achievements file path
 *****************************************************************************/
static void ra_get_mute_file_path(char* path, size_t path_size) {
	snprintf(path, path_size, USERDATA_PATH "/ra/muted/%s.txt", ra_game_hash);
}

/*****************************************************************************
 * Helper: Ensure mute directory exists
 *****************************************************************************/
static void ra_ensure_mute_dir(void) {
	char dir_path[512];
	snprintf(dir_path, sizeof(dir_path), USERDATA_PATH "/ra");
	mkdir(dir_path, 0755);
	snprintf(dir_path, sizeof(dir_path), USERDATA_PATH "/ra/muted");
	mkdir(dir_path, 0755);
}

/*****************************************************************************
 * Helper: Load muted achievements from file
 *****************************************************************************/
static void ra_load_muted_achievements(void) {
	ra_clear_muted_achievements();
	
	if (ra_game_hash[0] == '\0') {
		return;
	}
	
	char path[512];
	ra_get_mute_file_path(path, sizeof(path));
	
	FILE* f = fopen(path, "r");
	if (!f) {
		return;  // No mute file yet, that's okay
	}
	
	char line[32];
	while (fgets(line, sizeof(line), f) && ra_muted_count < RA_MAX_MUTED_ACHIEVEMENTS) {
		uint32_t id = (uint32_t)strtoul(line, NULL, 10);
		if (id > 0) {
			ra_muted_achievements[ra_muted_count++] = id;
		}
	}
	
	fclose(f);
	RA_LOG("Loaded %d muted achievements for game %s\n", ra_muted_count, ra_game_hash);
}

/*****************************************************************************
 * Helper: Save muted achievements to file
 *****************************************************************************/
static void ra_save_muted_achievements(void) {
	if (ra_game_hash[0] == '\0') {
		return;
	}
	
	if (!ra_muted_dirty) {
		return;  // Nothing changed
	}
	
	ra_ensure_mute_dir();
	
	char path[512];
	ra_get_mute_file_path(path, sizeof(path));
	
	// If no muted achievements, remove the file
	if (ra_muted_count == 0) {
		remove(path);
		ra_muted_dirty = false;
		return;
	}
	
	FILE* f = fopen(path, "w");
	if (!f) {
		RA_LOG("Error: Failed to save mute file: %s\n", path);
		return;
	}
	
	for (int i = 0; i < ra_muted_count; i++) {
		fprintf(f, "%u\n", ra_muted_achievements[i]);
	}
	
	fclose(f);
	ra_muted_dirty = false;
	RA_LOG("Saved %d muted achievements for game %s\n", ra_muted_count, ra_game_hash);
}

/*****************************************************************************
 * Helper: Clear muted achievements list
 *****************************************************************************/
static void ra_clear_muted_achievements(void) {
	ra_muted_count = 0;
	ra_muted_dirty = false;
}

/*****************************************************************************
 * Helper: Get core memory info callback for rc_libretro
 * 
 * This callback is used by rc_libretro_memory_init to query memory regions
 * from the libretro core when no memory map is provided.
 *****************************************************************************/

static void ra_get_core_memory_info(uint32_t id, rc_libretro_core_memory_info_t* info) {
	if (ra_get_memory_data && ra_get_memory_size) {
		info->data = (uint8_t*)ra_get_memory_data(id);
		info->size = ra_get_memory_size(id);
	} else {
		info->data = NULL;
		info->size = 0;
	}
}

/*****************************************************************************
 * Callback: Memory read
 * 
 * rcheevos calls this to read emulator memory for achievement checking.
 * We use rc_libretro_memory_read which handles memory maps properly.
 *****************************************************************************/

static uint32_t ra_read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
	(void)client; // unused
	
	// Use the properly initialized memory regions
	if (ra_memory_regions_initialized) {
		return rc_libretro_memory_read(&ra_memory_regions, address, buffer, num_bytes);
	}
	
	// Fallback for cases where memory regions aren't initialized yet
	// This shouldn't happen in normal operation, but provides backwards compatibility
	if (!ra_get_memory_data || !ra_get_memory_size) {
		return 0;
	}
	
	// RETRO_MEMORY_SYSTEM_RAM = 0
	void* mem_data = ra_get_memory_data(0);
	size_t mem_size = ra_get_memory_size(0);
	
	if (!mem_data || address + num_bytes > mem_size) {
		// Try save RAM as fallback (some cores expose different memory types)
		// RETRO_MEMORY_SAVE_RAM = 1
		mem_data = ra_get_memory_data(1);
		mem_size = ra_get_memory_size(1);
		
		if (!mem_data || address + num_bytes > mem_size) {
			return 0;
		}
	}
	
	memcpy(buffer, (uint8_t*)mem_data + address, num_bytes);
	return num_bytes;
}

/*****************************************************************************
 * Callback: Server call (HTTP)
 * 
 * rcheevos calls this for all server communication.
 * We use our HTTP wrapper to make async requests.
 * 
 * IMPORTANT: HTTP callbacks are invoked from worker threads. We queue the
 * responses and process them on the main thread in RA_idle() to avoid
 * race conditions with shared state (pending_game_load, notifications, etc).
 *****************************************************************************/

typedef struct {
	rc_client_server_callback_t callback;
	void* callback_data;
} RA_ServerCallData;

static void ra_http_callback(HTTP_Response* response, void* userdata) {
	RA_ServerCallData* data = (RA_ServerCallData*)userdata;
	
	// Extract response info before freeing
	const char* body = NULL;
	size_t body_length = 0;
	int http_status = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
	
	if (response && response->data && !response->error) {
		body = response->data;
		body_length = response->size;
		http_status = response->http_status;
	} else {
		// Error case
		if (response && response->error) {
			RA_LOG("HTTP error: %s\n", response->error);
		}
	}
	
	// Queue the response for main thread processing
	// The queue makes a copy of the body, so we can free the response after
	if (!ra_queue_push(body, body_length, http_status, data->callback, data->callback_data)) {
		// Queue failed (full or not initialized) - log but don't crash
		RA_LOG("Warning: Failed to queue HTTP response\n");
	}
	
	// Cleanup - safe to free now since queue copied the data
	if (response) {
		HTTP_freeResponse(response);
	}
	free(data);
}

static void ra_server_call(const rc_api_request_t* request,
                           rc_client_server_callback_t callback,
                           void* callback_data, rc_client_t* client) {
	(void)client; // unused
	
	// Allocate data structure to pass through to callback
	RA_ServerCallData* data = (RA_ServerCallData*)malloc(sizeof(RA_ServerCallData));
	if (!data) {
		// Out of memory - call callback with error
		rc_api_server_response_t error_response;
		memset(&error_response, 0, sizeof(error_response));
		error_response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
		callback(&error_response, callback_data);
		return;
	}
	
	data->callback = callback;
	data->callback_data = callback_data;
	
	// Make async HTTP request
	if (request->post_data && strlen(request->post_data) > 0) {
		HTTP_postAsync(request->url, request->post_data, request->content_type,
		               ra_http_callback, data);
	} else {
		HTTP_getAsync(request->url, ra_http_callback, data);
	}
}

/*****************************************************************************
 * Callback: Logging
 *****************************************************************************/

static void ra_log_message(const char* message, const rc_client_t* client) {
	(void)client;
	RA_LOG("%s\n", message);
}

/*****************************************************************************
 * Callback: Event handler
 * 
 * Called by rcheevos when achievements are unlocked, leaderboards triggered, etc.
 *****************************************************************************/

static void ra_event_handler(const rc_client_event_t* event, rc_client_t* client) {
	(void)client;
	char message[NOTIFICATION_MAX_MESSAGE];
	SDL_Surface* badge_icon = NULL;
	
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		// Hide "Unknown Emulator" notification when hardcore mode is disabled
		if (!CFG_getRAHardcoreMode() && event->achievement->id == 101000001) {
			RA_LOG("Skipping Unknown Emulator notification (not in hardcore mode)\n");
			break;
		}
		snprintf(message, sizeof(message), "Achievement Unlocked: %s",
		         event->achievement->title);
		// Get the unlocked badge icon (not locked)
		badge_icon = RA_Badges_getNotificationSize(event->achievement->badge_name, false);
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, badge_icon);
		RA_LOG("Achievement unlocked: %s (%d points)\n",
		       event->achievement->title, event->achievement->points);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		RA_LOG("Challenge started: %s\n", event->achievement->title);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		RA_LOG("Challenge ended: %s\n", event->achievement->title);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
		// Skip progress indicators if disabled (duration=0)
		if (CFG_getRAProgressNotificationDuration() == 0) {
			break;
		}
		// Skip progress indicators for muted achievements
		if (RA_isAchievementMuted(event->achievement->id)) {
			break;
		}
		// Show progress indicator with badge icon
		badge_icon = RA_Badges_getNotificationSize(event->achievement->badge_name, false);
		Notification_showProgressIndicator(
			event->achievement->title,
			event->achievement->measured_progress,
			badge_icon
		);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
		// Skip progress indicators if disabled (duration=0)
		if (CFG_getRAProgressNotificationDuration() == 0) {
			break;
		}
		// Skip progress indicators for muted achievements
		if (RA_isAchievementMuted(event->achievement->id)) {
			break;
		}
		// Update progress indicator with new value
		badge_icon = RA_Badges_getNotificationSize(event->achievement->badge_name, false);
		Notification_showProgressIndicator(
			event->achievement->title,
			event->achievement->measured_progress,
			badge_icon
		);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
		Notification_hideProgressIndicator();
		break;
		
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		snprintf(message, sizeof(message), "Leaderboard: %s",
		         event->leaderboard->title);
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		RA_LOG("Leaderboard started: %s\n", event->leaderboard->title);
		break;
		
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		RA_LOG("Leaderboard failed: %s\n", event->leaderboard->title);
		break;
		
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		snprintf(message, sizeof(message), "Submitted %s to %s",
		         event->leaderboard->tracker_value, event->leaderboard->title);
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		RA_LOG("Leaderboard submitted: %s - %s\n",
		       event->leaderboard->title, event->leaderboard->tracker_value);
		break;
		
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		Notification_push(NOTIFICATION_ACHIEVEMENT, "Game Mastered!", NULL);
		RA_LOG("Game mastered!\n");
		break;
		
	case RC_CLIENT_EVENT_RESET:
		RA_LOG("Reset requested (hardcore mode enabled)\n");
		break;
		
	case RC_CLIENT_EVENT_SERVER_ERROR:
		RA_LOG("Server error: %s\n",
		       event->server_error ? event->server_error->error_message : "unknown");
		// Show notification for server errors
		snprintf(message, sizeof(message), "RA Server Error: %s",
		         event->server_error ? event->server_error->error_message : "unknown");
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		break;
		
	case RC_CLIENT_EVENT_DISCONNECTED:
		RA_LOG("Disconnected - unlocks pending\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, "RetroAchievements: Offline mode", NULL);
		break;
		
	case RC_CLIENT_EVENT_RECONNECTED:
		RA_LOG("Reconnected - pending unlocks submitted\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, "RetroAchievements: Reconnected", NULL);
		break;
		
	default:
		RA_LOG("Unhandled event type: %d\n", event->type);
		break;
	}
}

/*****************************************************************************
 * Callback: Login callback
 *****************************************************************************/

static void ra_login_callback(int result, const char* error_message,
                              rc_client_t* client, void* userdata) {
	(void)userdata;
	
	if (result == RC_OK) {
		// Success - reset retry state
		ra_reset_login_state();
		ra_logged_in = true;
		
		const rc_client_user_t* user = rc_client_get_user_info(client);
		RA_LOG("Logged in as %s (score: %u)\n",
		       user ? user->display_name : "unknown",
		       user ? user->score : 0);
		
		// Check if we have a pending game to load
		if (pending_game_load) {
			RA_LOG("Processing deferred game load: %s\n", pending_rom_path);
			ra_do_load_game(pending_rom_path, pending_rom_data, pending_rom_size, pending_emu_tag);
			ra_clear_pending_game();
		}
	} else {
		// Failure - attempt retry or give up
		ra_logged_in = false;
		RA_LOG("Login failed: %s\n", error_message ? error_message : "unknown error");
		
		if (ra_login_retry_count < RA_LOGIN_MAX_RETRIES) {
			// Schedule retry
			uint32_t delay = ra_get_retry_delay_ms(ra_login_retry_count);
			ra_login_retry_time = SDL_GetTicks() + delay;
			ra_login_retry_pending = true;
			ra_login_retry_count++;
			
			RA_LOG("Scheduling retry %d/%d in %ums\n", 
			       ra_login_retry_count, RA_LOGIN_MAX_RETRIES, delay);
			
			// Show "Connecting..." notification on first retry only
			if (ra_login_retry_count == 1 && !ra_login_notified_connecting) {
				ra_login_notified_connecting = true;
				Notification_push(NOTIFICATION_ACHIEVEMENT, 
				                  "Connecting to RetroAchievements...", NULL);
			}
		} else {
			// All retries exhausted
			RA_LOG("All login retries exhausted\n");
			Notification_push(NOTIFICATION_ACHIEVEMENT, 
			                  "RetroAchievements: Connection failed", NULL);
			ra_reset_login_state();
			ra_clear_pending_game();
		}
	}
}

/*****************************************************************************
 * Helper: Prefetch all achievement badges for the loaded game
 *****************************************************************************/
static void ra_prefetch_badges(rc_client_t* client) {
	// Get the achievement list
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(client,
		RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
		RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	
	if (!list) {
		RA_LOG("Failed to get achievement list for badge prefetch\n");
		return;
	}
	
	// Collect all unique badge names
	int badge_count = 0;
	for (uint32_t b = 0; b < list->num_buckets; b++) {
		badge_count += list->buckets[b].num_achievements;
	}
	
	if (badge_count == 0) {
		rc_client_destroy_achievement_list(list);
		return;
	}
	
	const char** badge_names = (const char**)malloc(badge_count * sizeof(const char*));
	if (!badge_names) {
		rc_client_destroy_achievement_list(list);
		return;
	}
	
	int idx = 0;
	for (uint32_t b = 0; b < list->num_buckets; b++) {
		for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
			const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
			if (ach->badge_name[0] != '\0') {
				badge_names[idx++] = ach->badge_name;
			}
		}
	}
	
	// Prefetch all badges
	RA_Badges_prefetch(badge_names, idx);
	
	free(badge_names);
	rc_client_destroy_achievement_list(list);
	
	RA_LOG("Prefetching %d achievement badges\n", idx);
}

/*****************************************************************************
 * Callback: Game load callback
 *****************************************************************************/

static void ra_game_loaded_callback(int result, const char* error_message,
                                    rc_client_t* client, void* userdata) {
	(void)userdata;
	
	if (result == RC_OK) {
		const rc_client_game_t* game = rc_client_get_game_info(client);
		ra_game_loaded = true;
		
		if (game && game->id != 0) {
			RA_LOG("Game loaded: %s (ID: %u)\n", game->title, game->id);
			
			// Store game hash for mute file path
			if (game->hash && game->hash[0] != '\0') {
				strncpy(ra_game_hash, game->hash, sizeof(ra_game_hash) - 1);
				ra_game_hash[sizeof(ra_game_hash) - 1] = '\0';
			} else {
				// Fallback to game ID if no hash available
				snprintf(ra_game_hash, sizeof(ra_game_hash), "%u", game->id);
			}
			
			// Load muted achievements for this game
			ra_load_muted_achievements();
			
			// Initialize badge cache and prefetch achievement badges
			RA_Badges_init();
			ra_prefetch_badges(client);
			
			// Show achievement summary
			rc_client_user_game_summary_t summary;
			rc_client_get_user_game_summary(client, &summary);
			
			uint32_t display_unlocked = summary.num_unlocked_achievements;
			uint32_t display_total = summary.num_core_achievements;
			
			// Hide "Unknown Emulator" warning (ID 101000001) when hardcore mode is disabled.
			// Note: We intentionally show "Unsupported Game Version" so users know to find a supported ROM.
			if (!CFG_getRAHardcoreMode()) {
				rc_client_achievement_list_t* list = rc_client_create_achievement_list(
					client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
					RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
				if (list) {
					bool found = false;
					for (uint32_t b = 0; b < list->num_buckets && !found; b++) {
						for (uint32_t a = 0; a < list->buckets[b].num_achievements && !found; a++) {
							const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
							if (ach->id == 101000001) {
								// Subtract from total
								if (display_total > 0) display_total--;
								// If it's unlocked, subtract from unlocked count too
								if (ach->unlocked && display_unlocked > 0) display_unlocked--;
								found = true;
							}
						}
					}
					rc_client_destroy_achievement_list(list);
				}
			}
			
			char message[NOTIFICATION_MAX_MESSAGE];
			snprintf(message, sizeof(message), "%s - %u/%u achievements",
			         game->title, display_unlocked, display_total);
			Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		} else {
			RA_LOG("Game not recognized by RetroAchievements\n");
		}
	} else {
		ra_game_loaded = false;
		RA_LOG("Game load failed: %s\n", error_message ? error_message : "unknown error");
	}
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

void RA_init(void) {
	if (!CFG_getRAEnable()) {
		RA_LOG("RetroAchievements disabled in settings\n");
		return;
	}
	
	if (ra_client) {
		RA_LOG("Already initialized\n");
		return;
	}
	
	// Check wifi state before attempting to connect
	if (!PLAT_wifiEnabled()) {
		RA_LOG("WiFi disabled - cannot connect to RetroAchievements\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, 
		                  "RetroAchievements requires WiFi", NULL);
		return;
	}
	
	// Wait for wifi to connect (handles wake-from-sleep scenario)
	if (!PLAT_wifiConnected()) {
		RA_LOG("WiFi enabled but not connected, waiting up to %dms...\n", RA_WIFI_WAIT_MAX_MS);
		uint32_t start = SDL_GetTicks();
		while (!PLAT_wifiConnected() && 
		       (SDL_GetTicks() - start) < RA_WIFI_WAIT_MAX_MS) {
			SDL_Delay(RA_WIFI_WAIT_POLL_MS);
		}
		
		if (!PLAT_wifiConnected()) {
			RA_LOG("WiFi did not connect within %dms\n", RA_WIFI_WAIT_MAX_MS);
			Notification_push(NOTIFICATION_ACHIEVEMENT, 
			                  "RetroAchievements requires WiFi", NULL);
			return;
		}
		RA_LOG("WiFi connected after %ums\n", SDL_GetTicks() - start);
	}
	
	RA_LOG("Initializing...\n");
	
	// Initialize the response queue (must be before any HTTP requests)
	ra_queue_init();
	
	// Create rc_client with our callbacks
	ra_client = rc_client_create(ra_read_memory, ra_server_call);
	if (!ra_client) {
		RA_LOG("Failed to create rc_client\n");
		return;
	}
	
	// Configure logging
	rc_client_enable_logging(ra_client, RC_CLIENT_LOG_LEVEL_INFO, ra_log_message);
	
	// Set event handler
	rc_client_set_event_handler(ra_client, ra_event_handler);
	
	// Initialize and register CHD-aware CD reader for disc game hashing
	ra_init_cdreader();
	{
		rc_hash_callbacks_t hash_callbacks;
		memset(&hash_callbacks, 0, sizeof(hash_callbacks));
		
		// Set up CHD-aware CD reader callbacks
		hash_callbacks.cdreader.open_track = ra_cdreader_open_track;
		hash_callbacks.cdreader.open_track_iterator = ra_cdreader_open_track_iterator;
		hash_callbacks.cdreader.read_sector = ra_cdreader_read_sector;
		hash_callbacks.cdreader.close_track = ra_cdreader_close_track;
		hash_callbacks.cdreader.first_track_sector = ra_cdreader_first_track_sector;
		
		rc_client_set_hash_callbacks(ra_client, &hash_callbacks);
		RA_LOG("CHD disc image support enabled\n");
	}
	
	// Configure hardcore mode from settings
	rc_client_set_hardcore_enabled(ra_client, CFG_getRAHardcoreMode() ? 1 : 0);
	
	// Reset login state before attempting
	ra_reset_login_state();
	
	// Attempt login with stored token
	if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0) {
		RA_LOG("Logging in with stored token...\n");
		ra_start_login();
	} else {
		RA_LOG("No stored token - user needs to authenticate in settings\n");
	}
}

void RA_quit(void) {
	// Clear any pending game data
	ra_clear_pending_game();
	
	// Reset login retry state
	ra_reset_login_state();
	
	// Clean up badge cache
	RA_Badges_quit();
	
	// Clean up memory regions
	if (ra_memory_regions_initialized) {
		rc_libretro_memory_destroy(&ra_memory_regions);
		ra_memory_regions_initialized = false;
	}
	
	// Free our deep-copied memory map
	if (ra_memory_map_descriptors) {
		free(ra_memory_map_descriptors);
		ra_memory_map_descriptors = NULL;
	}
	if (ra_memory_map) {
		free(ra_memory_map);
		ra_memory_map = NULL;
	}
	
	if (ra_client) {
		RA_LOG("Shutting down...\n");
		rc_client_destroy(ra_client);
		ra_client = NULL;
	}
	
	// Clean up the response queue (after rc_client is destroyed)
	ra_queue_quit();
	
	ra_game_loaded = false;
	ra_logged_in = false;
}

void RA_setMemoryAccessors(RA_GetMemoryFunc get_data, RA_GetMemorySizeFunc get_size) {
	ra_get_memory_data = get_data;
	ra_get_memory_size = get_size;
}

void RA_setMemoryMap(const void* mmap) {
	// Free any existing memory map copy
	if (ra_memory_map_descriptors) {
		free(ra_memory_map_descriptors);
		ra_memory_map_descriptors = NULL;
	}
	if (ra_memory_map) {
		free(ra_memory_map);
		ra_memory_map = NULL;
	}
	
	if (!mmap) {
		RA_LOG("Memory map cleared\n");
		return;
	}
	
	// Deep copy the memory map since the core's data may be on the stack or freed later
	const struct retro_memory_map* src = (const struct retro_memory_map*)mmap;
	
	if (src->num_descriptors == 0 || !src->descriptors) {
		RA_LOG("Memory map has no descriptors\n");
		return;
	}
	
	// Allocate our copy of the memory map structure
	ra_memory_map = (struct retro_memory_map*)malloc(sizeof(struct retro_memory_map));
	if (!ra_memory_map) {
		RA_LOG("Failed to allocate memory map\n");
		return;
	}
	
	// Allocate and copy the descriptors array
	size_t desc_size = src->num_descriptors * sizeof(struct retro_memory_descriptor);
	ra_memory_map_descriptors = (struct retro_memory_descriptor*)malloc(desc_size);
	if (!ra_memory_map_descriptors) {
		free(ra_memory_map);
		ra_memory_map = NULL;
		RA_LOG("Failed to allocate memory map descriptors\n");
		return;
	}
	
	memcpy(ra_memory_map_descriptors, src->descriptors, desc_size);
	ra_memory_map->num_descriptors = src->num_descriptors;
	ra_memory_map->descriptors = ra_memory_map_descriptors;
	
	RA_LOG("Memory map set by core: %u descriptors (deep copied)\n", ra_memory_map->num_descriptors);
}

void RA_initMemoryRegions(uint32_t console_id) {
	// Clean up any existing regions
	if (ra_memory_regions_initialized) {
		rc_libretro_memory_destroy(&ra_memory_regions);
		ra_memory_regions_initialized = false;
	}
	
	// Initialize memory regions based on console type and available memory info
	memset(&ra_memory_regions, 0, sizeof(ra_memory_regions));
	
	int result = rc_libretro_memory_init(&ra_memory_regions, ra_memory_map,
	                                     ra_get_core_memory_info, console_id);
	
	if (result) {
		ra_memory_regions_initialized = true;
		RA_LOG("Memory regions initialized: %u regions, %zu total bytes\n",
		       ra_memory_regions.count, ra_memory_regions.total_size);
	} else {
		RA_LOG("Warning: Failed to initialize memory regions for console %u\n", console_id);
	}
}

/*****************************************************************************
 * Helper: Clear pending game data
 *****************************************************************************/
static void ra_clear_pending_game(void) {
	if (pending_rom_data) {
		free(pending_rom_data);
		pending_rom_data = NULL;
	}
	pending_rom_size = 0;
	pending_rom_path[0] = '\0';
	pending_emu_tag[0] = '\0';
	pending_game_load = false;
}

/*****************************************************************************
 * Helper: Check if a file extension indicates a CD image
 *****************************************************************************/
static int ra_is_cd_extension(const char* path) {
	if (!path) return 0;
	
	const char* ext = strrchr(path, '.');
	if (!ext) return 0;
	ext++; // skip the dot
	
	// Common CD image extensions
	return (strcasecmp(ext, "chd") == 0 ||
	        strcasecmp(ext, "cue") == 0 ||
	        strcasecmp(ext, "ccd") == 0 ||
	        strcasecmp(ext, "toc") == 0 ||
	        strcasecmp(ext, "m3u") == 0);
}

/*****************************************************************************
 * Helper: Actually load the game (internal, assumes logged in)
 *****************************************************************************/
static void ra_do_load_game(const char* rom_path, const uint8_t* rom_data, size_t rom_size, const char* emu_tag) {
	int console_id = RA_getConsoleId(emu_tag);
	if (console_id == RC_CONSOLE_UNKNOWN) {
		RA_LOG("Unknown console for tag '%s' - achievements disabled\n", emu_tag);
		return;
	}
	
	// Handle consoles that have separate CD variants
	// PCE tag is used for both HuCard and CD games in NextUI
	if (console_id == RC_CONSOLE_PC_ENGINE && ra_is_cd_extension(rom_path)) {
		console_id = RC_CONSOLE_PC_ENGINE_CD;
		RA_LOG("Detected PC Engine CD image, using console ID %d\n", console_id);
	}
	// MD tag is used for both cartridge and Sega CD games in NextUI
	else if (console_id == RC_CONSOLE_MEGA_DRIVE && ra_is_cd_extension(rom_path)) {
		console_id = RC_CONSOLE_SEGA_CD;
		RA_LOG("Detected Sega CD image, using console ID %d\n", console_id);
	}
	
	RA_LOG("Loading game: %s (console: %s, ID: %d)\n",
	       rom_path, rc_console_name(console_id), console_id);
	
	// Initialize memory regions for this console type BEFORE loading the game
	// This ensures rcheevos can read memory correctly when checking achievements
	RA_initMemoryRegions((uint32_t)console_id);
	
	// Use rc_client_begin_identify_and_load_game which hashes and identifies the ROM
#ifdef RC_CLIENT_SUPPORTS_HASH
	rc_client_begin_identify_and_load_game(ra_client, console_id,
		rom_path, rom_data, rom_size,
		ra_game_loaded_callback, NULL);
#else
	// Fallback for builds without hash support
	RA_LOG("Hash support not compiled in - cannot identify game\n");
#endif
}

void RA_loadGame(const char* rom_path, const uint8_t* rom_data, size_t rom_size, const char* emu_tag) {
	if (!ra_client || !CFG_getRAEnable()) {
		return;
	}
	
	// If not logged in yet, store the game info for deferred loading
	if (!ra_logged_in) {
		RA_LOG("Login in progress - deferring game load for: %s\n", rom_path);
		
		// Clear any previous pending game
		ra_clear_pending_game();
		
		// Store the path
		strncpy(pending_rom_path, rom_path, RA_MAX_PATH - 1);
		pending_rom_path[RA_MAX_PATH - 1] = '\0';
		
		// Store the emu tag
		strncpy(pending_emu_tag, emu_tag, sizeof(pending_emu_tag) - 1);
		pending_emu_tag[sizeof(pending_emu_tag) - 1] = '\0';
		
		// Copy ROM data if provided (some cores need it)
		if (rom_data && rom_size > 0) {
			pending_rom_data = (uint8_t*)malloc(rom_size);
			if (pending_rom_data) {
				memcpy(pending_rom_data, rom_data, rom_size);
				pending_rom_size = rom_size;
			} else {
				RA_LOG("Warning: Failed to allocate memory for pending ROM data\n");
				pending_rom_size = 0;
			}
		}
		
		pending_game_load = true;
		return;
	}
	
	// Already logged in - load immediately
	ra_do_load_game(rom_path, rom_data, rom_size, emu_tag);
}

void RA_unloadGame(void) {
	if (!ra_client) {
		return;
	}
	
	if (ra_game_loaded) {
		RA_LOG("Unloading game\n");
		
		// Save any pending muted achievements
		ra_save_muted_achievements();
		ra_clear_muted_achievements();
		ra_game_hash[0] = '\0';
		
		// Clear badge cache memory (keeps disk cache)
		RA_Badges_clearMemory();
		
		// Clean up memory regions for this game
		if (ra_memory_regions_initialized) {
			rc_libretro_memory_destroy(&ra_memory_regions);
			ra_memory_regions_initialized = false;
		}
		
		// Clear the memory map (will be set fresh when next game loads)
		// Note: We don't free here - the core may still be loaded and the map
		// will be needed if the same core loads another game. The memory is
		// freed in RA_quit() or overwritten in RA_setMemoryMap().
		
		rc_client_unload_game(ra_client);
		ra_game_loaded = false;
	}
}

void RA_doFrame(void) {
	// Process any pending HTTP responses before checking achievements
	// This ensures game load completes and achievements are active
	ra_process_queued_responses();
	
	if (ra_client && ra_game_loaded) {
		rc_client_do_frame(ra_client);
	}
}

void RA_idle(void) {
	// Process queued HTTP responses on main thread
	// This must happen even if ra_client is NULL (e.g., during shutdown)
	// to avoid memory leaks from pending responses
	ra_process_queued_responses();
	
	if (!ra_client) {
		return;
	}
	
	// Check for pending login retry
	if (ra_login_retry_pending && SDL_GetTicks() >= ra_login_retry_time) {
		ra_login_retry_pending = false;
		ra_start_login();
	}
	
	rc_client_idle(ra_client);
	
	// Process any responses that arrived during rc_client_idle()
	// This ensures callbacks from login/game load complete promptly
	ra_process_queued_responses();
}

bool RA_isGameLoaded(void) {
	return ra_game_loaded;
}

bool RA_isHardcoreModeActive(void) {
	if (!ra_client || !ra_game_loaded) {
		return false;
	}
	return rc_client_get_hardcore_enabled(ra_client) != 0;
}

bool RA_isLoggedIn(void) {
	return ra_logged_in;
}

const char* RA_getUserDisplayName(void) {
	if (!ra_client || !ra_logged_in) {
		return NULL;
	}
	const rc_client_user_t* user = rc_client_get_user_info(ra_client);
	return user ? user->display_name : NULL;
}

const char* RA_getGameTitle(void) {
	if (!ra_client || !ra_game_loaded) {
		return NULL;
	}
	const rc_client_game_t* game = rc_client_get_game_info(ra_client);
	return game ? game->title : NULL;
}

void RA_getAchievementSummary(uint32_t* unlocked, uint32_t* total) {
	if (!ra_client || !ra_game_loaded) {
		if (unlocked) *unlocked = 0;
		if (total) *total = 0;
		return;
	}
	
	// Get counts from the actual achievement list to ensure consistency
	// between displayed count and what's shown in the achievements menu
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(
		ra_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
		RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	
	uint32_t unlocked_count = 0;
	uint32_t total_count = 0;
	
	if (list) {
		bool hide_unknown_emulator = !CFG_getRAHardcoreMode();
		
		for (uint32_t b = 0; b < list->num_buckets; b++) {
			for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
				const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
				// Skip "Unknown Emulator" warning when hardcore mode is disabled
				if (hide_unknown_emulator && ach->id == 101000001) {
					continue;
				}
				total_count++;
				if (ach->unlocked) {
					unlocked_count++;
				}
			}
		}
		rc_client_destroy_achievement_list(list);
	}
	
	if (unlocked) *unlocked = unlocked_count;
	if (total) *total = total_count;
}

const void* RA_createAchievementList(int category, int grouping) {
	if (!ra_client || !ra_game_loaded) {
		return NULL;
	}
	return rc_client_create_achievement_list(ra_client, category, grouping);
}

void RA_destroyAchievementList(const void* list) {
	if (list) {
		rc_client_destroy_achievement_list((rc_client_achievement_list_t*)list);
	}
}

const char* RA_getGameHash(void) {
	if (!ra_game_loaded || ra_game_hash[0] == '\0') {
		return NULL;
	}
	return ra_game_hash;
}

bool RA_isAchievementMuted(uint32_t achievement_id) {
	for (int i = 0; i < ra_muted_count; i++) {
		if (ra_muted_achievements[i] == achievement_id) {
			return true;
		}
	}
	return false;
}

bool RA_toggleAchievementMute(uint32_t achievement_id) {
	if (RA_isAchievementMuted(achievement_id)) {
		RA_setAchievementMuted(achievement_id, false);
		return false;
	} else {
		RA_setAchievementMuted(achievement_id, true);
		return true;
	}
}

void RA_setAchievementMuted(uint32_t achievement_id, bool muted) {
	if (muted) {
		// Add to muted list if not already there
		if (!RA_isAchievementMuted(achievement_id)) {
			if (ra_muted_count < RA_MAX_MUTED_ACHIEVEMENTS) {
				ra_muted_achievements[ra_muted_count++] = achievement_id;
				ra_muted_dirty = true;
				RA_LOG("Achievement %u muted\n", achievement_id);
			} else {
				RA_LOG("Warning: Max muted achievements reached, cannot mute %u\n", achievement_id);
			}
		}
	} else {
		// Remove from muted list
		for (int i = 0; i < ra_muted_count; i++) {
			if (ra_muted_achievements[i] == achievement_id) {
				// Shift remaining elements down
				for (int j = i; j < ra_muted_count - 1; j++) {
					ra_muted_achievements[j] = ra_muted_achievements[j + 1];
				}
				ra_muted_count--;
				ra_muted_dirty = true;
				RA_LOG("Achievement %u unmuted\n", achievement_id);
				break;
			}
		}
	}
}

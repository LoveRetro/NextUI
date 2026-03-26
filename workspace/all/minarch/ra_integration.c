#include "ra_integration.h"
#include "ra_consoles.h"
#include "chd_reader.h"
#include "config.h"
#include "http.h"
#include "notification.h"
#include "ra_badges.h"
#include "ra_offline.h"
#include "ra_sync.h"
#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <SDL2/SDL.h>

#include <rcheevos/rc_client.h>
#include <rcheevos/rc_libretro.h>
#include <rcheevos/rc_hash.h>
#include <rcheevos/rc_api_user.h>

// Logging macros - use NextUI log levels
#define RA_LOG_DEBUG(fmt, ...) LOG_debug("[RA] " fmt, ##__VA_ARGS__)
#define RA_LOG_INFO(fmt, ...)  LOG_info("[RA] " fmt, ##__VA_ARGS__)
#define RA_LOG_WARN(fmt, ...)  LOG_warn("[RA] " fmt, ##__VA_ARGS__)
#define RA_LOG_ERROR(fmt, ...) LOG_error("[RA] " fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Static state
 *****************************************************************************/

static rc_client_t* ra_client = NULL;
static bool ra_game_loaded = false;
static bool ra_logged_in = false;
// (Deferred flags are in RADeferredState below)

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
typedef struct {
	char rom_path[RA_MAX_PATH];
	uint8_t* rom_data;
	size_t rom_size;
	char emu_tag[16];
	bool active;
} RAPendingLoad;

static RAPendingLoad ra_pending_load = {0};

// Login retry state
#define RA_LOGIN_MAX_RETRIES 5
typedef struct {
	int count;
	uint32_t next_time;           // SDL_GetTicks() timestamp for next retry
	bool pending;
	bool notified_connecting;     // Track if we showed "Connecting..." notification
} RALoginRetry;

static RALoginRetry ra_login_retry = {0};

// Wifi wait config
#define RA_WIFI_WAIT_MAX_MS 3000   // 3 seconds max blocking wait
#define RA_WIFI_WAIT_POLL_MS 500   // Check every 500ms

// Connectivity probe config
#define RA_PROBE_INTERVAL_MS  30000  // 30 seconds between probe attempts
#define RA_PROBE_SLEEP_CHUNK_MS 200  // Sleep granularity for abort responsiveness

// Connectivity probe state (background thread polls RA login endpoint)
static volatile bool ra_probe_abort = false;
static volatile bool ra_probe_running = false;

// Deferred state: flags set by background threads (probe, sync), consumed
// by the main thread in ra_process_deferred_flags().  Protected by
// ra_deferred.mutex — all reads and writes must hold the lock.
#define RA_MAX_DEFERRED_SYNC_IDS 256
typedef struct {
	SDL_mutex* mutex;
	bool offline_notification;
	bool online_notification;
	bool sync;
	bool hardcore_enable;
	bool hardcore_disable;
	bool sync_apply;
	uint32_t sync_ids[RA_MAX_DEFERRED_SYNC_IDS];
	uint32_t sync_count;
	bool user_saw_offline;
} RADeferredState;

static RADeferredState ra_deferred = {0};

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

// Forward declarations for deferred state lifecycle
static void ra_deferred_init(void);
static void ra_deferred_quit(void);

// Forward declarations for helper functions
static void ra_clear_pending_game(void);
static void ra_do_load_game(const char* rom_path, const uint8_t* rom_data, size_t rom_size, const char* emu_tag);
static void ra_load_muted_achievements(void);
static void ra_save_muted_achievements(void);
static void ra_clear_muted_achievements(void);
static void ra_reset_login_state(void);
static void ra_start_login(void);
static void ra_start_offline_sync(uint32_t game_id);
static uint32_t ra_get_retry_delay_ms(int attempt);
static void ra_login_callback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ra_start_connectivity_probe(void);
static void ra_stop_connectivity_probe(void);

// Extracted helpers to reduce duplication
static void ra_interruptible_sleep(uint32_t ms, volatile bool* abort_flag);
static bool ra_get_post_param(const char* post_data, char param_key, char* buf, size_t buf_size);
static bool ra_should_hide_achievement(const rc_client_achievement_t* ach);
static uint32_t ra_reapply_pending_unlocks(rc_client_t* client, const char* game_hash);
static void ra_show_game_summary(rc_client_t* client, const rc_client_game_t* game);

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
	
	RA_LOG_DEBUG("Initializing CHD-aware CD reader\n");
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
	ra_login_retry.count = 0;
	ra_login_retry.pending = false;
	ra_login_retry.next_time = 0;
	ra_login_retry.notified_connecting = false;
}

/*****************************************************************************
 * Helper: Start a login attempt
 *****************************************************************************/
static void ra_start_login(void) {
	RA_LOG_DEBUG("Attempting login (attempt %d/%d)...\n", 
	       ra_login_retry.count + 1, RA_LOGIN_MAX_RETRIES);
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
		RA_LOG_WARN("Response queue full, dropping response\n");
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
	int processed = 0;
	
	while (ra_queue_pop(&resp)) {
		processed++;
		RA_LOG_DEBUG("Processing queued response #%d: http_status=%d, body_len=%zu\n",
		            processed, resp.http_status_code, resp.body_length);
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
 * Deferred state lifecycle
 *
 * ra_deferred_init / ra_deferred_quit manage the mutex that protects the
 * RADeferredState struct.  Called from RA_init / RA_quit alongside the
 * response-queue lifecycle.
 *****************************************************************************/

static void ra_deferred_init(void) {
	if (!ra_deferred.mutex) {
		ra_deferred.mutex = SDL_CreateMutex();
	}
	// Zero all flags (struct is already zero-initialized, but be explicit
	// in case RA_init is called more than once in the process lifetime).
	ra_deferred.offline_notification = false;
	ra_deferred.online_notification = false;
	ra_deferred.sync = false;
	ra_deferred.hardcore_enable = false;
	ra_deferred.hardcore_disable = false;
	ra_deferred.sync_apply = false;
	ra_deferred.sync_count = 0;
	ra_deferred.user_saw_offline = false;
}

static void ra_deferred_quit(void) {
	if (ra_deferred.mutex) {
		SDL_DestroyMutex(ra_deferred.mutex);
		ra_deferred.mutex = NULL;
	}
}

/*****************************************************************************
 * Helper: Muted achievements file path
 *****************************************************************************/
static void ra_get_mute_file_path(char* path, size_t path_size) {
	snprintf(path, path_size, SHARED_USERDATA_PATH "/.ra/muted/%s.txt", ra_game_hash);
}

/*****************************************************************************
 * Helper: Ensure mute directory exists
 *****************************************************************************/
static void ra_ensure_mute_dir(void) {
	char dir_path[512];
	snprintf(dir_path, sizeof(dir_path), SHARED_USERDATA_PATH "/.ra");
	mkdir(dir_path, 0755);
	snprintf(dir_path, sizeof(dir_path), SHARED_USERDATA_PATH "/.ra/muted");
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
	RA_LOG_DEBUG("Loaded %d muted achievements for game %s\n", ra_muted_count, ra_game_hash);
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
		RA_LOG_ERROR("Error: Failed to save mute file: %s\n", path);
		return;
	}
	
	for (int i = 0; i < ra_muted_count; i++) {
		fprintf(f, "%u\n", ra_muted_achievements[i]);
	}
	
	fclose(f);
	ra_muted_dirty = false;
	RA_LOG_DEBUG("Saved %d muted achievements for game %s\n", ra_muted_count, ra_game_hash);
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
 * Helpers: POST parameter extraction, achievement filtering
 *****************************************************************************/

/**
 * Extract a single-character parameter value from URL-encoded POST data.
 * For example, ra_get_post_param("r=login&u=foo", 'r', buf, 32) writes "login" to buf.
 * Returns true if found, false otherwise.
 */
static bool ra_get_post_param(const char* post_data, char param_key,
                              char* buf, size_t buf_size) {
	if (!post_data || !buf || buf_size == 0) return false;
	buf[0] = '\0';
	
	char needle[4] = { param_key, '=', '\0' };
	const char* p = post_data;
	while ((p = strstr(p, needle)) != NULL) {
		if (p == post_data || *(p - 1) == '&') {
			const char* val = p + 2;
			const char* end = val;
			while (*end && *end != '&') end++;
			size_t len = (size_t)(end - val);
			if (len >= buf_size) len = buf_size - 1;
			memcpy(buf, val, len);
			buf[len] = '\0';
			return true;
		}
		p += 2;
	}
	return false;
}

/**
 * Returns true if this achievement should be hidden from the user.
 * Currently hides the "Unknown Emulator" warning (ID 101000001) when
 * hardcore mode is disabled.
 */
static bool ra_should_hide_achievement(const rc_client_achievement_t* ach) {
	return !CFG_getRAHardcoreMode() && ach->id == 101000001;
}

/**
 * Re-apply pending offline unlock state to rcheevos' internal achievement data.
 *
 * When hardcore mode is re-enabled after an offline session, rcheevos clears
 * achievements that only have the softcore unlock bit (which is all offline
 * unlocks, since offline forces softcore). This function reads the pending
 * unlock ledger, finds achievements matching the given game hash, and sets
 * both the softcore and hardcore unlock bits so they remain visible as unlocked.
 *
 * Also useful at game-load time: if startsession patching missed some pending
 * unlocks (e.g., due to a race with the connectivity probe), this ensures they
 * are marked unlocked in rcheevos immediately.
 *
 * Returns the number of achievements whose state was changed.
 */
static uint32_t ra_reapply_pending_unlocks(rc_client_t* client, const char* game_hash) {
	if (!client || !game_hash || !game_hash[0]) return 0;
	
	RA_PendingUnlock* pending = NULL;
	uint32_t pending_count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&pending, &pending_count) ||
	    pending_count == 0 || !pending) {
		return 0;
	}
	
	uint32_t fixed = 0;
	for (uint32_t i = 0; i < pending_count; i++) {
		if (strcmp(pending[i].game_hash, game_hash) != 0) continue;
		
		const rc_client_achievement_t* ach =
			rc_client_get_achievement_info(client, pending[i].achievement_id);
		if (!ach) continue;
		
		// Only fix achievements that rcheevos thinks are still locked
		if (ach->state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
			// Cast away const — rc_client_get_achievement_info returns a const
			// pointer to the internal struct, but we need to update unlock state.
			// This is safe: the data is mutable and we're on the main thread.
			rc_client_achievement_t* m = (rc_client_achievement_t*)ach;
			m->unlocked |= RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH;
			m->unlock_time = (time_t)pending[i].timestamp;
			m->state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			fixed++;
		}
	}
	
	free(pending);
	return fixed;
}

/**
 * Compute the game achievement summary (unlocked/total), augmented with
 * pending offline unlocks and filtered to hide suppressed achievements,
 * then push the result as a notification.
 */
static void ra_show_game_summary(rc_client_t* client, const rc_client_game_t* game) {
	rc_client_user_game_summary_t summary;
	rc_client_get_user_game_summary(client, &summary);
	
	uint32_t display_unlocked = summary.num_unlocked_achievements;
	uint32_t display_total = summary.num_core_achievements;
	
	// Re-apply any pending offline unlocks that rcheevos may not know
	// about yet (e.g., startsession patching was skipped due to timing,
	// or hardcore re-enable cleared softcore-only unlock bits).
	// This both fixes rcheevos' internal state and returns the count
	// so we can augment the notification.
	uint32_t extra = ra_reapply_pending_unlocks(client, game->hash);
	display_unlocked += extra;
	if (display_unlocked > display_total) {
		display_unlocked = display_total;
	}
	if (extra > 0) {
		RA_LOG_INFO("Notification: added %u pending offline unlocks to count\n", extra);
	}

	// Hide filtered achievements (e.g., "Unknown Emulator" in non-hardcore mode)
	// from the summary counts.
	// Note: We intentionally show "Unsupported Game Version" so users know to find a supported ROM.
	{
		rc_client_achievement_list_t* list = rc_client_create_achievement_list(
			client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
			RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
		if (list) {
			for (uint32_t b = 0; b < list->num_buckets; b++) {
				for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
					const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
					if (ra_should_hide_achievement(ach)) {
						if (display_total > 0) display_total--;
						if (ach->unlocked && display_unlocked > 0) display_unlocked--;
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
	char* url;        /* Owned copy for write-through caching */
	char* post_data;  /* Owned copy for write-through caching (may be NULL) */
} RA_ServerCallData;

static void ra_http_callback(HTTP_Response* response, void* userdata) {
	RA_ServerCallData* data = (RA_ServerCallData*)userdata;
	
	// Extract response info before freeing
	const char* body = NULL;
	size_t body_length = 0;
	// Default to RETRYABLE error so rcheevos will retry on network failures
	// (connection refused, timeout, DNS failure, etc.)
	int http_status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
	
	if (response && response->data && !response->error) {
		body = response->data;
		body_length = response->size;
		http_status = response->http_status;
		
		// Write-through cache: store successful responses for offline use
		if (http_status == 200 && body_length > 0) {
			RA_Offline_cacheResponse(data->url, data->post_data, body, body_length);
			
		// SYNC_ACK: when an online awardachievement succeeds, mark it in the ledger
		// so the sync engine won't re-submit it after a crash.
		// Verify the response body indicates success — a 200 with "Success":false
		// (e.g., rate limit, invalid achievement) must NOT generate a SYNC_ACK
		// or the unlock would be silently dropped from the ledger.
		if (data->post_data && strstr(data->post_data, "r=awardachievement") &&
		    strstr(body, "\"Success\":true")) {
				char a_buf[16] = {0};
				if (ra_get_post_param(data->post_data, 'a', a_buf, sizeof(a_buf))) {
					uint32_t ach_id = (uint32_t)strtoul(a_buf, NULL, 10);
					if (ach_id > 0) {
						RA_Offline_ledgerWriteSyncAck(ach_id, 0);
						RA_Offline_removePendingCacheEntry(ach_id);
						// Invalidate cached startsession so the next
						// offline-first launch sees this unlock in the
						// server's fresh response instead of a stale cache.
						RA_Offline_invalidateStartsessionCache();
					}
				}
			}
		}
	} else {
		// Error case — network failure, timeout, DNS error, etc.
		// Extract request type only (don't log full post_data — it may contain API tokens)
		char err_rt_buf[32] = {0};
		const char* err_rtype = ra_get_post_param(data->post_data, 'r', err_rt_buf, sizeof(err_rt_buf))
		                        ? err_rt_buf : "unknown";
		if (response && response->error) {
			RA_LOG_ERROR("HTTP error for r=%s: %s\n", err_rtype, response->error);
		} else {
			RA_LOG_ERROR("HTTP error for r=%s: no response\n", err_rtype);
		}
	}
	
	// For online startsession responses, patch in any pending ledger unlocks
	// so rcheevos shows offline-earned achievements as unlocked even before
	// the sync engine submits them to the server.
	char* patched_body = NULL;
	size_t patched_length = 0;
	if (body && body_length > 0 && http_status == 200 &&
	    data->post_data && strstr(data->post_data, "r=startsession")) {
		// Make a mutable copy for patching
		patched_body = (char*)malloc(body_length + 1);
		if (patched_body) {
			memcpy(patched_body, body, body_length);
			patched_body[body_length] = '\0';
			patched_length = body_length;
			
			// Extract game hash from request params
			char game_hash[64] = {0};
			ra_get_post_param(data->post_data, 'm', game_hash, sizeof(game_hash));
			
			if (game_hash[0] && RA_Offline_patchStartsessionResponse(
			        patched_body, patched_length, game_hash,
			        &patched_body, &patched_length)) {
				RA_LOG_INFO("Patched online startsession with pending ledger unlocks\n");
				body = patched_body;
				body_length = patched_length;
			} else {
				// No patching needed or failed — use original
				free(patched_body);
				patched_body = NULL;
			}
		}
	}
	
	// Queue the response for main thread processing
	// The queue makes a copy of the body, so we can free the response after
	if (!ra_queue_push(body, body_length, http_status, data->callback, data->callback_data)) {
		// Queue failed (full or not initialized) - log but don't crash
		RA_LOG_WARN("Failed to queue HTTP response\n");
	}
	
	// Cleanup - safe to free now since queue copied the data
	if (patched_body) {
		free(patched_body);
	}
	if (response) {
		HTTP_freeResponse(response);
	}
	free(data->url);
	free(data->post_data);
	free(data);
}

static void ra_server_call(const rc_api_request_t* request,
                           rc_client_server_callback_t callback,
                           void* callback_data, rc_client_t* client) {
	(void)client; // unused
	
	// Offline mode: serve cached responses or return retryable errors
	if (RA_Offline_isOffline()) {
		// Extract request type for logging and decision-making
		char rt_buf[32] = {0};
		const char* req_type = ra_get_post_param(request->post_data, 'r', rt_buf, sizeof(rt_buf))
		                       ? rt_buf : NULL;
		
		// Try cache first — this handles login, gameid, achievementsets, startsession, patch
		char* cached_body = NULL;
		size_t cached_len = 0;
		
		if (RA_Offline_getCachedResponse(request->url, request->post_data,
		                                  &cached_body, &cached_len)) {
			// Cache hit - deliver cached response via queue
			RA_LOG_DEBUG("Offline cache hit: %s (%zu bytes)\n",
			             req_type ? req_type : "unknown", cached_len);
			if (!ra_queue_push(cached_body, cached_len, 200, callback, callback_data)) {
				RA_LOG_WARN("Failed to queue cached response\n");
			}
			free(cached_body);
			return;
		}
		
		// Cache miss — behavior depends on request type.
		// For non-cacheable requests (awardachievement, ping, submitlbentry, etc.),
		// return a retryable error so rcheevos keeps them in its retry queue.
		// This prevents false RECONNECTED events when rcheevos retries succeed
		// against our synthetic responses while the network is still down.
		// The connectivity probe handles the real reconnection detection.
		//
		// For cacheable request types without a cache entry (e.g., startsession
		// for a never-before-played game while offline), we still synthesize
		// {"Success":true} so rcheevos can proceed with the game load.
		bool is_cacheable_type = false;
		if (req_type) {
			is_cacheable_type = (strcmp(req_type, "login2") == 0 ||
			                     strcmp(req_type, "gameid") == 0 ||
			                     strcmp(req_type, "achievementsets") == 0 ||
			                     strcmp(req_type, "startsession") == 0 ||
			                     strcmp(req_type, "patch") == 0);
		}
		
		if (is_cacheable_type) {
			// Cacheable type with cache miss — synthesize minimal success
			// so rcheevos can proceed with game load
			RA_LOG_DEBUG("Offline cache miss (cacheable): %s — returning synthetic success\n",
			             req_type);
			static const char* empty_success = "{\"Success\":true}";
			if (!ra_queue_push(empty_success, strlen(empty_success), 200, callback, callback_data)) {
				RA_LOG_WARN("Failed to queue synthetic offline response\n");
			}
		} else {
			// Non-cacheable type (awardachievement, ping, etc.) — return retryable error
			// so rcheevos keeps the request in its retry queue
			RA_LOG_DEBUG("Offline cache miss (non-cacheable): %s — returning retryable error\n",
			             req_type ? req_type : "unknown");
			rc_api_server_response_t error_response;
			memset(&error_response, 0, sizeof(error_response));
			error_response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
			// Direct callback is safe here: ra_server_call runs on the main thread,
			// and rcheevos has no internal threading — all server_call invocations
			// are synchronous on the calling thread.
			callback(&error_response, callback_data);
		}
		return;
	}
	
	// Online mode: make real HTTP request
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
	data->url = request->url ? strdup(request->url) : NULL;
	data->post_data = request->post_data ? strdup(request->post_data) : NULL;
	
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
	RA_LOG_DEBUG("%s\n", message);
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
		if (ra_should_hide_achievement(event->achievement)) {
			RA_LOG_DEBUG("Skipping hidden achievement notification\n");
			break;
		}
		snprintf(message, sizeof(message), "Achievement Unlocked: %s",
		         event->achievement->title);
		// Get the unlocked badge icon (not locked)
		badge_icon = RA_Badges_getNotificationSize(event->achievement->badge_name, false);
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, badge_icon);
		RA_LOG_INFO("Achievement unlocked: %s (%d points)\n",
		       event->achievement->title, event->achievement->points);
		
		// Write-ahead log: persist unlock to ledger (survives app crash)
		{
			const rc_client_game_t* game = rc_client_get_game_info(client);
			if (game) {
				RA_Offline_ledgerWriteUnlock(game->id, event->achievement->id,
				                             game->hash,
				                             rc_client_get_hardcore_enabled(client) ? 1 : 0);
				// Update pending cache so UI shows offline indicator immediately
				// (always add — if disconnect occurs before server confirms, the
				// indicator is already in place; sync engine clears it on success)
				RA_Offline_addPendingCacheEntry(event->achievement->id);
			}
		}
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		RA_LOG_DEBUG("Challenge started: %s\n", event->achievement->title);
		break;
		
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		RA_LOG_DEBUG("Challenge ended: %s\n", event->achievement->title);
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
		RA_LOG_INFO("Leaderboard started: %s\n", event->leaderboard->title);
		break;
		
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		RA_LOG_INFO("Leaderboard failed: %s\n", event->leaderboard->title);
		break;
		
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
		snprintf(message, sizeof(message), "Submitted %s to %s",
		         event->leaderboard->tracker_value, event->leaderboard->title);
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		RA_LOG_INFO("Leaderboard submitted: %s - %s\n",
		       event->leaderboard->title, event->leaderboard->tracker_value);
		break;
		
	case RC_CLIENT_EVENT_GAME_COMPLETED:
		Notification_push(NOTIFICATION_ACHIEVEMENT, "Game Mastered!", NULL);
		RA_LOG_INFO("Game mastered!\n");
		break;
		
	case RC_CLIENT_EVENT_RESET:
		RA_LOG_WARN("Reset requested (hardcore mode enabled)\n");
		break;
		
	case RC_CLIENT_EVENT_SERVER_ERROR:
		RA_LOG_ERROR("Server error: %s\n",
		       event->server_error ? event->server_error->error_message : "unknown");
		// Show notification for server errors
		snprintf(message, sizeof(message), "RA Server Error: %s",
		         event->server_error ? event->server_error->error_message : "unknown");
		Notification_push(NOTIFICATION_ACHIEVEMENT, message, NULL);
		break;
		
	case RC_CLIENT_EVENT_DISCONNECTED:
		RA_LOG_WARN("Disconnected - switching to offline mode\n");
		RA_Offline_setOffline(true);
		// Force softcore when offline
		rc_client_set_hardcore_enabled(client, 0);
		SDL_LockMutex(ra_deferred.mutex);
		ra_deferred.user_saw_offline = true;
		SDL_UnlockMutex(ra_deferred.mutex);
		// Start probe to detect when connectivity returns
		ra_start_connectivity_probe();
		break;
		
	case RC_CLIENT_EVENT_RECONNECTED:
		RA_LOG_INFO("Reconnected - switching to online mode\n");
		// Stop probe (redundant — rcheevos already confirmed connectivity)
		ra_stop_connectivity_probe();
		RA_Offline_setOffline(false);
		// Re-enable hardcore if configured
		if (CFG_getRAHardcoreMode()) {
			rc_client_set_hardcore_enabled(client, 1);
			RA_LOG_INFO("Hardcore re-enabled after reconnection\n");
			uint32_t fixed = ra_reapply_pending_unlocks(client, ra_game_hash);
			if (fixed > 0) {
				RA_LOG_INFO("Re-applied %u offline unlock(s) after "
				            "reconnect hardcore re-enable\n", fixed);
			}
		}
		SDL_LockMutex(ra_deferred.mutex);
		ra_deferred.user_saw_offline = false;
		SDL_UnlockMutex(ra_deferred.mutex);
		// Network is back — try syncing current game's ledger entries
		{
			const rc_client_game_t* g = rc_client_get_game_info(client);
			ra_start_offline_sync(g ? g->id : 0);
		}
		break;
		
	default:
		RA_LOG_DEBUG("Unhandled event type: %d\n", event->type);
		break;
	}
}

/*****************************************************************************
 * Callback: Login callback
 *****************************************************************************/

static void ra_login_callback(int result, const char* error_message,
                              rc_client_t* client, void* userdata) {
	(void)userdata;
	
	RA_LOG_INFO("ra_login_callback: result=%d, error=%s\n", result,
	            error_message ? error_message : "(none)");
	
	if (result == RC_OK) {
		// Success - reset retry state
		ra_reset_login_state();
		ra_logged_in = true;
		
		const rc_client_user_t* user = rc_client_get_user_info(client);
		RA_LOG_INFO("Logged in as %s (score: %u)\n",
		       user ? user->display_name : "unknown",
		       user ? user->score : 0);
		
		// Check if we have a pending game to load
		if (ra_pending_load.active) {
			RA_LOG_DEBUG("Processing deferred game load: %s\n", ra_pending_load.rom_path);
			ra_do_load_game(ra_pending_load.rom_path, ra_pending_load.rom_data, 
			                ra_pending_load.rom_size, ra_pending_load.emu_tag);
			ra_clear_pending_game();
		}
	} else {
		// Failure - attempt retry or give up
		ra_logged_in = false;
		RA_LOG_ERROR("Login failed: %s\n", error_message ? error_message : "unknown error");
		
		if (ra_login_retry.count < RA_LOGIN_MAX_RETRIES) {
			// Schedule retry
			uint32_t delay = ra_get_retry_delay_ms(ra_login_retry.count);
			ra_login_retry.next_time = SDL_GetTicks() + delay;
			ra_login_retry.pending = true;
			ra_login_retry.count++;
			
			RA_LOG_DEBUG("Scheduling retry %d/%d in %ums\n", 
			       ra_login_retry.count, RA_LOGIN_MAX_RETRIES, delay);
			
			// Show "Connecting..." notification on first retry only
			if (ra_login_retry.count == 1 && !ra_login_retry.notified_connecting) {
				ra_login_retry.notified_connecting = true;
				Notification_push(NOTIFICATION_ACHIEVEMENT, 
				                  "Connecting to RetroAchievements...", NULL);
			}
		} else {
			// All retries exhausted
			RA_LOG_ERROR("All login retries exhausted\n");
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
		RA_LOG_WARN("Failed to get achievement list for badge prefetch\n");
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
	
	RA_LOG_DEBUG("Prefetching %d achievement badges\n", idx);
}

/*****************************************************************************
 * Offline sync engine
 * 
 * When online, replays pending offline achievement unlocks to the server.
 * Runs on a background thread with realistic timing between submissions
 * to avoid looking automated.
 *****************************************************************************/

static SDL_Thread* ra_sync_thread = NULL;
static volatile bool ra_sync_abort = false;
static uint32_t ra_sync_game_id = 0;

/**
 * Background-thread–safe interruptible sleep.
 * Uses SDL_Delay in small chunks, checking the abort flag between each.
 */
static void ra_interruptible_sleep(uint32_t ms, volatile bool* abort_flag) {
	uint32_t slept = 0;
	while (slept < ms && !(*abort_flag)) {
		uint32_t chunk = (ms - slept > 200) ? 200 : (ms - slept);
		SDL_Delay(chunk);
		slept += chunk;
	}
}


/**
 * Background thread function: replay pending offline unlocks using
 * the shared sync engine (ra_sync.c).
 */
static int ra_sync_thread_func(void* data) {
	(void)data;
	
	// Reset deferred sync-apply state for this sync session
	SDL_LockMutex(ra_deferred.mutex);
	ra_deferred.sync_count = 0;
	ra_deferred.sync_apply = false;
	SDL_UnlockMutex(ra_deferred.mutex);
	
	// Snapshot achievement IDs that are pending BEFORE sync, so we can
	// tell the main thread which ones were synced (after sync, the ledger
	// may have been compacted and the records are gone).
	RA_PendingUnlock* pre_unlocks = NULL;
	uint32_t pre_count = 0;
	RA_Offline_ledgerGetPendingUnlocks(&pre_unlocks, &pre_count);
	
	// Show sync progress in top-left (same area as badge download notifications)
	if (pre_count > 0) {
		char msg[NOTIFICATION_MAX_MESSAGE];
		snprintf(msg, sizeof(msg), "Syncing %u offline achievement%s...",
		         pre_count, pre_count == 1 ? "" : "s");
		Notification_setProgressIndicatorPersistent(true);
		Notification_showProgressIndicator(msg, "", NULL);
	}
	
	// Run the shared sync engine with background timing
	RA_SyncConfig config = RA_SYNC_CONFIG_BACKGROUND;
	RA_SyncResult result = RA_Sync_syncAll(ra_sync_game_id, &config,
	                                       &ra_sync_abort, NULL, NULL);
	
	RA_LOG_INFO("Sync complete: %u synced, %u skipped, %u failed (of %u)\n",
	            result.synced, result.skipped, result.failed, result.total);
	
	// Store synced achievement IDs for deferred main-thread state update.
	// We know that RA_Sync_syncAll processes unlocks in order from the
	// pending list, and synced + skipped + failed <= total. The first
	// (synced + skipped) entries in our pre-snapshot were processed.
	// We collect the IDs that match the game filter and were successfully
	// synced so the main thread can update rcheevos unlock bits.
	if (result.synced > 0 && pre_unlocks && pre_count > 0) {
		SDL_LockMutex(ra_deferred.mutex);
		uint32_t idx = 0;
		uint32_t processed = 0;
		for (uint32_t i = 0; i < pre_count && idx < RA_MAX_DEFERRED_SYNC_IDS; i++) {
			// Skip entries that don't match the game filter
			if (ra_sync_game_id != 0 && pre_unlocks[i].game_id != ra_sync_game_id)
				continue;
			// Skip hardcore (not synced)
			if (pre_unlocks[i].hardcore)
				continue;
			// The sync processes in order: synced entries come first,
			// then skipped, then failed (which stops the loop).
			// We can't distinguish synced vs skipped by position alone,
			// but we know exactly result.synced were successful.
			// Since the sync thread writes SYNC_ACK for each success,
			// just store all IDs up to result.synced count.
			processed++;
			if (processed <= result.synced) {
				ra_deferred.sync_ids[idx++] = pre_unlocks[i].achievement_id;
			}
		}
		ra_deferred.sync_count = idx;
		ra_deferred.sync_apply = true;
		SDL_UnlockMutex(ra_deferred.mutex);
	}
	
	free(pre_unlocks);
	
	// Show completion in top-left progress area, then auto-hide
	{
		char msg[NOTIFICATION_MAX_MESSAGE];
		if (result.failed > 0) {
			snprintf(msg, sizeof(msg), "Sync incomplete: %u synced, will retry later",
			         result.synced);
		} else if (result.synced > 0) {
			snprintf(msg, sizeof(msg), "Synced %u offline achievement%s",
			         result.synced, result.synced == 1 ? "" : "s");
		} else if (result.skipped > 0) {
			snprintf(msg, sizeof(msg), "Sync: %u achievement%s skipped",
			         result.skipped, result.skipped == 1 ? "" : "s");
		}
		if (result.synced > 0 || result.failed > 0 || result.skipped > 0) {
			Notification_setProgressIndicatorPersistent(false);
			Notification_showProgressIndicator(msg, "", NULL);
		} else {
			Notification_hideProgressIndicator();
		}
	}
	
	// If sync failed, go back to offline mode and restart the probe
	// so connectivity can be re-verified and sync retried
	if (result.failed > 0) {
		RA_Offline_setOffline(true);
		SDL_LockMutex(ra_deferred.mutex);
		ra_deferred.user_saw_offline = true;
		ra_deferred.offline_notification = true;
		ra_deferred.hardcore_disable = true;
		SDL_UnlockMutex(ra_deferred.mutex);
		ra_start_connectivity_probe();
	}
	
	RA_Offline_setSyncing(false);
	return 0;
}

/**
 * Start offline sync if we're online and have pending unlocks.
 * Called after successful online game load or connectivity restoration.
 * 
 * @param game_id  Game to sync (0 = sync all games).
 */
static void ra_start_offline_sync(uint32_t game_id) {
	if (RA_Offline_isOffline()) {
		RA_LOG_DEBUG("Sync: skipped (offline mode active)\n");
		return;
	}
	if (RA_Offline_isSyncing()) {
		RA_LOG_DEBUG("Sync: skipped (already syncing)\n");
		return;
	}
	
	// Quick check: any pending unlocks?
	uint32_t count = 0;
	if (!RA_Sync_hasPendingUnlocks(&count) || count == 0) {
		RA_LOG_DEBUG("Sync: no pending unlocks found in ledger\n");
		return;
	}
	
	// Start sync on background thread
	RA_LOG_INFO("Starting offline sync (%u pending unlocks, game_id=%u)\n",
	            count, game_id);
	ra_sync_abort = false;
	ra_sync_game_id = game_id;
	RA_Offline_setSyncing(true);
	ra_sync_thread = SDL_CreateThread(ra_sync_thread_func, "ra_sync", NULL);
	if (!ra_sync_thread) {
		RA_LOG_ERROR("Failed to create sync thread\n");
		RA_Offline_setSyncing(false);
	} else {
		SDL_DetachThread(ra_sync_thread);
		ra_sync_thread = NULL;
	}
}


/*****************************************************************************
 * Connectivity probe
 * 
 * When starting in offline mode, a background thread periodically attempts
 * a login request to the RA server. On success, it flips offline mode off
 * and sets deferred flags so the main thread can push notifications, start
 * sync, and re-enable hardcore. The probe uses HTTP_post() synchronously
 * (background thread) and rc_api_init_login_request() to build the
 * request — it does NOT call rc_client_begin_login (which rejects
 * concurrent login attempts and accesses non-thread-safe client state).
 *****************************************************************************/

/**
 * Background thread function: periodically probe RA login endpoint.
 * On success, cache the response, flip to online, set deferred flags, exit.
 * On failure, sleep RA_PROBE_INTERVAL_MS and retry.
 * No retry limit — probes until success or ra_probe_abort.
 */
static int ra_connectivity_probe_func(void* data) {
	(void)data;
	
	RA_LOG_INFO("Connectivity probe started\n");
	
	const char* username = CFG_getRAUsername();
	const char* token = CFG_getRAToken();
	
	if (!username || !token || strlen(username) == 0 || strlen(token) == 0) {
		RA_LOG_ERROR("Probe: no credentials available, exiting\n");
		ra_probe_running = false;
		return 1;
	}
	
	while (!ra_probe_abort) {
		// Build login request via rcheevos API
		rc_api_login_request_t login_params;
		memset(&login_params, 0, sizeof(login_params));
		login_params.username = username;
		login_params.api_token = token;
		
		rc_api_request_t request;
		memset(&request, 0, sizeof(request));
		
		int rc = rc_api_init_login_request(&request, &login_params);
		if (rc != RC_OK) {
			RA_LOG_ERROR("Probe: failed to build login request (rc=%d)\n", rc);
			rc_api_destroy_request(&request);
			ra_probe_running = false;
			return 1;
		}
		
		RA_LOG_DEBUG("Probe: attempting login...\n");
		
		// Synchronous HTTP POST (background thread, blocking OK)
		HTTP_Response* http_resp = HTTP_post(request.url, request.post_data,
		                                     request.content_type);
		
		bool success = false;
		if (http_resp && http_resp->http_status == 200 &&
		    http_resp->data && http_resp->size > 0) {
			// Check for "Success":true in response
			if (strstr(http_resp->data, "\"Success\":true")) {
				// Cache the login response (write-through)
				RA_Offline_cacheResponse(request.url, request.post_data,
				                         http_resp->data, http_resp->size);
				
				// Flip to online mode
				RA_Offline_setOffline(false);
				
				// Set deferred flags for main thread to handle.
				// If the offline notification hasn't been displayed yet
				// (still deferred), cancel it — the user never saw "Offline"
				// so showing "Connected" would also be meaningless noise.
				SDL_LockMutex(ra_deferred.mutex);
				if (ra_deferred.offline_notification) {
					ra_deferred.offline_notification = false;
					RA_LOG_INFO("Probe: cancelled pending offline notification (connectivity arrived in time)\n");
					// Don't set online_notification — user never saw offline
				} else {
					ra_deferred.online_notification = true;
				}
				ra_deferred.sync = true;
				if (CFG_getRAHardcoreMode()) {
					ra_deferred.hardcore_enable = true;
				}
				SDL_UnlockMutex(ra_deferred.mutex);
				
				success = true;
				RA_LOG_INFO("Probe: login successful, transitioning to online mode\n");
			} else {
				RA_LOG_WARN("Probe: server returned 200 but Success!=true\n");
			}
		} else {
			RA_LOG_DEBUG("Probe: login failed (status=%d)\n",
			             http_resp ? http_resp->http_status : -1);
		}
		
		if (http_resp) HTTP_freeResponse(http_resp);
		rc_api_destroy_request(&request);
		
		if (success || ra_probe_abort) break;
		
		ra_interruptible_sleep(RA_PROBE_INTERVAL_MS, &ra_probe_abort);
	}
	
	RA_LOG_INFO("Connectivity probe exiting\n");
	ra_probe_running = false;
	return 0;
}

/**
 * Start the connectivity probe thread (if not already running).
 */
static void ra_start_connectivity_probe(void) {
	if (ra_probe_running) {
		RA_LOG_DEBUG("Probe: already running, not starting another\n");
		return;
	}
	
	ra_probe_abort = false;
	ra_probe_running = true;
	
	SDL_Thread* thread = SDL_CreateThread(ra_connectivity_probe_func, "ra_probe", NULL);
	if (!thread) {
		RA_LOG_ERROR("Failed to create connectivity probe thread\n");
		ra_probe_running = false;
		return;
	}
	
	SDL_DetachThread(thread);
	RA_LOG_INFO("Connectivity probe thread launched\n");
}

/**
 * Stop the connectivity probe thread (if running).
 * Blocks up to ~1s waiting for the thread to exit.
 */
static void ra_stop_connectivity_probe(void) {
	if (!ra_probe_running) return;
	
	RA_LOG_DEBUG("Stopping connectivity probe...\n");
	ra_probe_abort = true;
	
	// Wait for thread to notice and exit (up to 1 second)
	for (int i = 0; i < 20 && ra_probe_running; i++) {
		SDL_Delay(50);
	}
	
	if (ra_probe_running) {
		RA_LOG_WARN("Probe thread did not exit within 1s, proceeding anyway\n");
	}
}

/*****************************************************************************
 * Callback: Game load callback
 *****************************************************************************/

static void ra_game_loaded_callback(int result, const char* error_message,
                                    rc_client_t* client, void* userdata) {
	(void)userdata;
	
	RA_LOG_INFO("ra_game_loaded_callback: result=%d, error=%s\n", result,
	            error_message ? error_message : "(none)");
	
	if (result == RC_OK) {
		const rc_client_game_t* game = rc_client_get_game_info(client);
		ra_game_loaded = true;
		
		if (game && game->id != 0) {
			RA_LOG_INFO("Game loaded: %s (ID: %u)\n", game->title, game->id);
			
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
			
			// Refresh pending offline unlock cache (for UI display)
			RA_Offline_refreshPendingCache();
			
			// Initialize badge cache and prefetch achievement badges
			RA_Badges_init();
			ra_prefetch_badges(client);
			
			// Show achievement summary (includes offline unlock augmentation
			// and filtered achievement hiding)
			ra_show_game_summary(client, game);
			
			// Ledger: record session start
			RA_Offline_ledgerWriteSessionStart(game->id, game->hash,
			                                   rc_client_get_hardcore_enabled(client) ? 1 : 0);
			
			// Sync pending offline unlocks for this game (runs in background)
			ra_start_offline_sync(game->id);
		} else {
			RA_LOG_WARN("Game not recognized by RetroAchievements\n");
			// Still try to sync — game may not be recognized but we may have
			// pending unlocks from a prior offline session with a different game
			ra_start_offline_sync(0);
		}
	} else {
		ra_game_loaded = false;
		RA_LOG_ERROR("Game load failed: %s\n", error_message ? error_message : "unknown error");
		// Try to sync even on load failure — we're online (or partially) and may
		// have pending unlocks from prior offline sessions
		ra_start_offline_sync(0);
	}
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

void RA_init(void) {
	RA_LOG_INFO("RA_init() called, RAEnable=%d\n", CFG_getRAEnable());
	if (!CFG_getRAEnable()) {
		RA_LOG_INFO("RetroAchievements disabled in settings, returning early\n");
		return;
	}
	
	if (ra_client) {
		RA_LOG_INFO("Already initialized, returning early\n");
		return;
	}
	
	// Initialize offline subsystem (always, regardless of WiFi state)
	RA_Offline_init(SHARED_USERDATA_PATH);
	
	// Determine startup mode: offline-first (with probe) vs online vs pure offline
	bool wifi_enabled = PLAT_wifiEnabled();
	bool start_offline = false;
	bool launch_probe = false;
	
	RA_LOG_INFO("WiFi check: enabled=%d, connected=%d\n",
	            wifi_enabled, wifi_enabled ? PLAT_wifiConnected() : 0);
	
	if (wifi_enabled) {
		// WiFi is on — check if we have a cached login for offline-first startup
		char* cached_login = NULL;
		size_t cached_len = 0;
		// Build a probe-style login request to get the correct cache key
		bool has_cached_login = false;
		if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0) {
			rc_api_login_request_t login_params;
			memset(&login_params, 0, sizeof(login_params));
			login_params.username = CFG_getRAUsername();
			login_params.api_token = CFG_getRAToken();
			
			rc_api_request_t request;
			memset(&request, 0, sizeof(request));
			if (rc_api_init_login_request(&request, &login_params) == RC_OK) {
				has_cached_login = RA_Offline_getCachedResponse(
					request.url, request.post_data, &cached_login, &cached_len);
				if (has_cached_login) {
					free(cached_login);
				}
				rc_api_destroy_request(&request);
			}
		}
		
		if (has_cached_login) {
			// Cache exists — start offline immediately, probe in background
			RA_LOG_INFO("Cached login found — offline-first startup with background probe\n");
			start_offline = true;
			launch_probe = true;
		} else {
			// No cache — must go online for login (current behavior with blocking wait)
			RA_LOG_INFO("No cached login — online startup with WiFi wait\n");
			if (!PLAT_wifiConnected()) {
				RA_LOG_DEBUG("WiFi enabled but not connected, waiting up to %dms...\n", RA_WIFI_WAIT_MAX_MS);
				uint32_t start = SDL_GetTicks();
				while (!PLAT_wifiConnected() &&
				       (SDL_GetTicks() - start) < RA_WIFI_WAIT_MAX_MS) {
					SDL_Delay(RA_WIFI_WAIT_POLL_MS);
				}
				if (PLAT_wifiConnected()) {
					RA_LOG_DEBUG("WiFi connected after %ums\n", SDL_GetTicks() - start);
				} else {
					RA_LOG_WARN("WiFi did not connect within %dms - cannot start online, no cache available\n", RA_WIFI_WAIT_MAX_MS);
					start_offline = true;  // pure offline, no probe (no cache to serve from)
				}
			}
			// If WiFi connected and no cache, start_offline stays false → online path
		}
	} else {
		// WiFi radio off — pure offline, no probe
		RA_LOG_INFO("WiFi disabled — pure offline mode\n");
		start_offline = true;
	}
	
	RA_Offline_setOffline(start_offline);
	
	// Initialize the deferred state (must be before setting any flags)
	ra_deferred_init();
	
	if (start_offline) {
		RA_LOG_INFO("Initializing in offline mode (softcore only)...\n");
		// Defer notification — Notification_init() hasn't been called yet at this point
		SDL_LockMutex(ra_deferred.mutex);
		ra_deferred.offline_notification = true;
		SDL_UnlockMutex(ra_deferred.mutex);
	} else {
		RA_LOG_INFO("Initializing in online mode...\n");
	}
	
	// Initialize the response queue (must be before any HTTP requests)
	ra_queue_init();
	
	// Create rc_client with our callbacks
	ra_client = rc_client_create(ra_read_memory, ra_server_call);
	if (!ra_client) {
		RA_LOG_ERROR("Failed to create rc_client\n");
		return;
	}
	
	// Configure logging
	rc_client_enable_logging(ra_client, RC_CLIENT_LOG_LEVEL_WARN, ra_log_message);
	
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
		RA_LOG_DEBUG("CHD disc image support enabled\n");
	}
	
	// Configure hardcore mode from settings
	// Force softcore when offline
	if (RA_Offline_isOffline()) {
		rc_client_set_hardcore_enabled(ra_client, 0);
	} else {
		rc_client_set_hardcore_enabled(ra_client, CFG_getRAHardcoreMode() ? 1 : 0);
	}
	
	// Reset login state before attempting
	ra_reset_login_state();
	
	// Attempt login with stored token
	RA_LOG_INFO("Credentials check: authenticated=%d, token_len=%zu, username=%s\n",
	            CFG_getRAAuthenticated(), strlen(CFG_getRAToken()), CFG_getRAUsername());
	if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0) {
		RA_LOG_INFO("Logging in with stored token (offline=%d)...\n", RA_Offline_isOffline());
		ra_start_login();
		
		// Launch connectivity probe if we started offline with a cached login
		if (launch_probe) {
			ra_start_connectivity_probe();
		}
	} else {
		RA_LOG_WARN("No stored token - user needs to authenticate in settings\n");
	}
	RA_LOG_INFO("RA_init() complete\n");
}

void RA_quit(void) {
	// Abort connectivity probe
	ra_stop_connectivity_probe();
	
	// Abort any running sync
	if (RA_Offline_isSyncing()) {
		RA_LOG_INFO("Aborting offline sync for shutdown\n");
		ra_sync_abort = true;
		for (int i = 0; i < 20 && RA_Offline_isSyncing(); i++) {
			SDL_Delay(50);
		}
	}
	
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
		RA_LOG_INFO("Shutting down...\n");
		rc_client_destroy(ra_client);
		ra_client = NULL;
	}
	
	// Clean up the response queue (after rc_client is destroyed)
	ra_queue_quit();
	
	// Clean up deferred state mutex
	ra_deferred_quit();
	
	// Shut down offline subsystem (after everything else)
	RA_Offline_shutdown();
	
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
		RA_LOG_DEBUG("Memory map cleared\n");
		return;
	}
	
	// Deep copy the memory map since the core's data may be on the stack or freed later
	const struct retro_memory_map* src = (const struct retro_memory_map*)mmap;
	
	if (src->num_descriptors == 0 || !src->descriptors) {
		RA_LOG_WARN("Memory map has no descriptors\n");
		return;
	}
	
	// Allocate our copy of the memory map structure
	ra_memory_map = (struct retro_memory_map*)malloc(sizeof(struct retro_memory_map));
	if (!ra_memory_map) {
		RA_LOG_ERROR("Failed to allocate memory map\n");
		return;
	}
	
	// Allocate and copy the descriptors array
	size_t desc_size = src->num_descriptors * sizeof(struct retro_memory_descriptor);
	ra_memory_map_descriptors = (struct retro_memory_descriptor*)malloc(desc_size);
	if (!ra_memory_map_descriptors) {
		free(ra_memory_map);
		ra_memory_map = NULL;
		RA_LOG_ERROR("Failed to allocate memory map descriptors\n");
		return;
	}
	
	memcpy(ra_memory_map_descriptors, src->descriptors, desc_size);
	ra_memory_map->num_descriptors = src->num_descriptors;
	ra_memory_map->descriptors = ra_memory_map_descriptors;
	
	RA_LOG_DEBUG("Memory map set by core: %u descriptors (deep copied)\n", ra_memory_map->num_descriptors);
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
		RA_LOG_DEBUG("Memory regions initialized: %u regions, %zu total bytes\n",
		       ra_memory_regions.count, ra_memory_regions.total_size);
	} else {
		RA_LOG_WARN("Failed to initialize memory regions for console %u\n", console_id);
	}
}

/*****************************************************************************
 * Helper: Clear pending game data
 *****************************************************************************/
static void ra_clear_pending_game(void) {
	if (ra_pending_load.rom_data) {
		free(ra_pending_load.rom_data);
		ra_pending_load.rom_data = NULL;
	}
	ra_pending_load.rom_size = 0;
	ra_pending_load.rom_path[0] = '\0';
	ra_pending_load.emu_tag[0] = '\0';
	ra_pending_load.active = false;
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
		RA_LOG_WARN("Unknown console for tag '%s' - achievements disabled\n", emu_tag);
		return;
	}
	
	// Handle consoles that have separate CD variants
	// PCE tag is used for both HuCard and CD games in NextUI
	if (console_id == RC_CONSOLE_PC_ENGINE && ra_is_cd_extension(rom_path)) {
		console_id = RC_CONSOLE_PC_ENGINE_CD;
		RA_LOG_DEBUG("Detected PC Engine CD image, using console ID %d\n", console_id);
	}
	// MD tag is used for both cartridge and Sega CD games in NextUI
	else if (console_id == RC_CONSOLE_MEGA_DRIVE && ra_is_cd_extension(rom_path)) {
		console_id = RC_CONSOLE_SEGA_CD;
		RA_LOG_DEBUG("Detected Sega CD image, using console ID %d\n", console_id);
	}
	
	RA_LOG_INFO("Loading game: %s (console: %s, ID: %d)\n",
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
	RA_LOG_ERROR("Hash support not compiled in - cannot identify game\n");
#endif
}

void RA_loadGame(const char* rom_path, const uint8_t* rom_data, size_t rom_size, const char* emu_tag) {
	RA_LOG_INFO("RA_loadGame: client=%p, RAEnable=%d, logged_in=%d, path=%s\n",
	            (void*)ra_client, CFG_getRAEnable(), ra_logged_in, rom_path);
	if (!ra_client || !CFG_getRAEnable()) {
		return;
	}
	
	// If not logged in yet, store the game info for deferred loading
	if (!ra_logged_in) {
		RA_LOG_DEBUG("Login in progress - deferring game load for: %s\n", rom_path);
		
		// Clear any previous pending game
		ra_clear_pending_game();
		
		// Store the path
		strncpy(ra_pending_load.rom_path, rom_path, RA_MAX_PATH - 1);
		ra_pending_load.rom_path[RA_MAX_PATH - 1] = '\0';
		
		// Store the emu tag
		strncpy(ra_pending_load.emu_tag, emu_tag, sizeof(ra_pending_load.emu_tag) - 1);
		ra_pending_load.emu_tag[sizeof(ra_pending_load.emu_tag) - 1] = '\0';
		
		// Copy ROM data if provided (some cores need it)
		if (rom_data && rom_size > 0) {
			ra_pending_load.rom_data = (uint8_t*)malloc(rom_size);
			if (ra_pending_load.rom_data) {
				memcpy(ra_pending_load.rom_data, rom_data, rom_size);
				ra_pending_load.rom_size = rom_size;
			} else {
				RA_LOG_WARN("Failed to allocate memory for pending ROM data\n");
				ra_pending_load.rom_size = 0;
			}
		}
		
		ra_pending_load.active = true;
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
		RA_LOG_INFO("Unloading game\n");
		
		// Abort connectivity probe and sync before unloading
		ra_stop_connectivity_probe();
		
		if (RA_Offline_isSyncing()) {
			RA_LOG_INFO("Aborting offline sync for game unload\n");
			ra_sync_abort = true;
			// Give sync thread time to notice the abort
			for (int i = 0; i < 10 && RA_Offline_isSyncing(); i++) {
				SDL_Delay(50);
			}
		}
		
		// Write SESSION_END to ledger before cleanup invalidates game info
		const rc_client_game_t* game = rc_client_get_game_info(ra_client);
		if (game) {
			RA_Offline_ledgerWriteSessionEnd(game->id, game->hash);
		}
		
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

/**
 * Process deferred state transition flags set by background threads.
 * Called periodically from RA_doFrame() (~every 500ms) and from RA_idle().
 * Handles: offline/online notifications, hardcore re-enable, sync trigger,
 * and login retry scheduling.
 */
static void ra_process_deferred_flags(void) {
	// --- Snapshot: copy all deferred flags under the lock, then reset them ---
	bool snap_offline_notification = false;
	bool snap_online_notification = false;
	bool snap_sync = false;
	bool snap_hardcore_enable = false;
	bool snap_hardcore_disable = false;
	bool snap_sync_apply = false;
	bool snap_user_saw_offline = false;
	uint32_t snap_sync_ids[RA_MAX_DEFERRED_SYNC_IDS];
	uint32_t snap_sync_count = 0;
	
	SDL_LockMutex(ra_deferred.mutex);
	
	snap_offline_notification = ra_deferred.offline_notification;
	snap_online_notification = ra_deferred.online_notification;
	snap_sync = ra_deferred.sync;
	snap_hardcore_enable = ra_deferred.hardcore_enable;
	snap_hardcore_disable = ra_deferred.hardcore_disable;
	snap_sync_apply = ra_deferred.sync_apply;
	snap_user_saw_offline = ra_deferred.user_saw_offline;
	snap_sync_count = ra_deferred.sync_count;
	if (snap_sync_apply && snap_sync_count > 0) {
		memcpy(snap_sync_ids, ra_deferred.sync_ids,
		       snap_sync_count * sizeof(uint32_t));
	}
	
	// Reset all consumed flags while still holding the lock
	ra_deferred.offline_notification = false;
	ra_deferred.online_notification = false;
	ra_deferred.sync = false;
	ra_deferred.hardcore_enable = false;
	ra_deferred.hardcore_disable = false;
	ra_deferred.sync_apply = false;
	if (snap_sync_apply) {
		ra_deferred.sync_count = 0;
	}
	// Update user_saw_offline based on the transitions we're about to process
	if (snap_offline_notification) {
		ra_deferred.user_saw_offline = true;
	}
	if (snap_online_notification && snap_user_saw_offline) {
		ra_deferred.user_saw_offline = false;
	}
	
	SDL_UnlockMutex(ra_deferred.mutex);
	
	// --- Process the snapshot without holding the lock ---
	
	// Process deferred offline flag (Notification_init() wasn't ready during RA_init())
	if (snap_offline_notification) {
		// user_saw_offline was already set inside the lock above
	}
	
	// Handle deferred online transition (set by connectivity probe thread)
	if (snap_online_notification) {
		// user_saw_offline was already cleared inside the lock above
	}
	
	if (snap_hardcore_enable) {
		if (ra_client && CFG_getRAHardcoreMode()) {
			rc_client_set_hardcore_enabled(ra_client, 1);
			RA_LOG_INFO("Hardcore re-enabled after online transition\n");
			uint32_t fixed = ra_reapply_pending_unlocks(ra_client, ra_game_hash);
			if (fixed > 0) {
				RA_LOG_INFO("Re-applied %u offline unlock(s) after "
				            "hardcore re-enable\n", fixed);
			}
		}
	}
	if (snap_hardcore_disable) {
		if (ra_client) {
			rc_client_set_hardcore_enabled(ra_client, 0);
			RA_LOG_INFO("Hardcore disabled after sync failure (offline mode)\n");
		}
	}
	if (snap_sync) {
		// Don't start sync until the game is loaded — the sync thread will
		// compact the ledger when done, which removes pending records needed
		// by startsession patching and ra_reapply_pending_unlocks. If we sync
		// before the game loads, those records are gone before rcheevos can
		// use them, causing the just-synced achievement to appear locked.
		if (!ra_game_loaded) {
			SDL_LockMutex(ra_deferred.mutex);
			ra_deferred.sync = true;  // Put it back for next cycle
			SDL_UnlockMutex(ra_deferred.mutex);
		} else {
			const rc_client_game_t* g = rc_client_get_game_info(ra_client);
			ra_start_offline_sync(g ? g->id : 0);
		}
	}
	
	// Apply synced achievement unlock state to rcheevos.
	// The sync thread confirmed these achievements with the RA server; now we
	// update rcheevos' internal unlock bits so the achievement list and summary
	// reflect the correct state without needing to restart the game.
	if (snap_sync_apply) {
		// If the game isn't loaded yet, rcheevos doesn't have achievement data
		// so we can't update unlock bits. Put the flags back for next cycle.
		if (!ra_game_loaded) {
			SDL_LockMutex(ra_deferred.mutex);
			ra_deferred.sync_apply = true;
			ra_deferred.sync_count = snap_sync_count;
			memcpy(ra_deferred.sync_ids, snap_sync_ids,
			       snap_sync_count * sizeof(uint32_t));
			SDL_UnlockMutex(ra_deferred.mutex);
		} else if (ra_client) {
			uint32_t applied = 0;
			uint8_t mode = rc_client_get_hardcore_enabled(ra_client)
				? RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH
				: RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE;
			
			for (uint32_t i = 0; i < snap_sync_count; i++) {
				const rc_client_achievement_t* ach =
					rc_client_get_achievement_info(ra_client, snap_sync_ids[i]);
				if (ach && !(ach->unlocked & mode)) {
					// Cast away const — rc_client_get_achievement_info returns a const
					// pointer to the internal struct, but we need to update unlock state.
					// This is safe: the data is mutable and we're on the main thread.
					rc_client_achievement_t* mutable_ach = (rc_client_achievement_t*)ach;
					mutable_ach->unlocked |= mode;
					mutable_ach->unlock_time = (time_t)time(NULL);
					if (mutable_ach->state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
						mutable_ach->state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
					}
					applied++;
				}
			}
			
			if (applied > 0) {
				RA_LOG_INFO("Applied %u synced achievement unlocks to rcheevos state\n", applied);
			}
		}
	}
	
	// Check for pending login retry
	if (ra_client && ra_login_retry.pending && SDL_GetTicks() >= ra_login_retry.next_time) {
		ra_login_retry.pending = false;
		ra_start_login();
	}
}

void RA_doFrame(void) {
	// Process any pending HTTP responses before checking achievements
	// This ensures game load completes and achievements are active
	ra_process_queued_responses();
	
	if (ra_client && ra_game_loaded) {
		rc_client_do_frame(ra_client);
	}
	
	// Periodically process deferred state transitions (~every 500ms)
	// This ensures connectivity probe results, login retries, and sync
	// triggers are handled during gameplay without waiting for menu open.
	// Cost: one SDL_GetTicks() + one integer comparison per frame.
	{
		static uint32_t last_deferred_check = 0;
		uint32_t now = SDL_GetTicks();
		if (now - last_deferred_check >= 500) {
			last_deferred_check = now;
			ra_process_deferred_flags();
		}
	}
}

void RA_idle(void) {
	// Process any deferred flags (also done periodically in RA_doFrame,
	// but running here too ensures prompt handling when menu is open)
	ra_process_deferred_flags();
	
	// Process queued HTTP responses on main thread
	// This must happen even if ra_client is NULL (e.g., during shutdown)
	// to avoid memory leaks from pending responses
	ra_process_queued_responses();
	
	if (!ra_client) {
		return;
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
		for (uint32_t b = 0; b < list->num_buckets; b++) {
			for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
				const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
				if (ra_should_hide_achievement(ach)) {
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
				RA_LOG_DEBUG("Achievement %u muted\n", achievement_id);
			} else {
				RA_LOG_WARN("Max muted achievements reached, cannot mute %u\n", achievement_id);
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
				RA_LOG_DEBUG("Achievement %u unmuted\n", achievement_id);
				break;
			}
		}
	}
}

bool RA_isAchievementOfflinePending(uint32_t achievement_id) {
	return RA_Offline_isUnlockPending(achievement_id);
}

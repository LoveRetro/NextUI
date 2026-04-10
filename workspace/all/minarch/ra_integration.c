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

#define RA_UTIL_NEED_SDL
#include "ra_util.h"
#include "ra_fsm.h"

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
 * State machine enums (SM-1 / SM-4)
 *
 * The codebase has 4 state machines.  For connectivity and sync the enums
 * are derived (computed from underlying flags on each query).  For login
 * and game-load the enum variable IS the authoritative state — every
 * transition is an explicit assignment.  Background threads communicate
 * exclusively through the FSM event queue (ra_fsm.h); the main thread
 * drains the queue in ra_process_deferred_flags() and updates the state
 * variables / deferred-action bools.
 *****************************************************************************/

/* Connectivity: is the device online or offline (with/without probe)? */
typedef enum {
	CONN_ONLINE,                  /* !isOffline && !probe_running            */
	CONN_OFFLINE_NO_PROBE,        /* isOffline && !probe_running             */
	CONN_OFFLINE_PROBING,         /* isOffline && probe_running && !abort    */
	CONN_OFFLINE_PROBE_STOPPING   /* isOffline && probe_running && abort     */
} RAConnState;

/* Login: lifecycle of the rc_client login attempt (authoritative). */
typedef enum {
	LOGIN_IDLE,              /* no credentials or not yet attempted          */
	LOGIN_IN_PROGRESS,       /* attempt in flight (async rc_client call)    */
	LOGIN_RETRY_WAITING,     /* failed, timer running before next retry     */
	LOGIN_LOGGED_IN,         /* authenticated                               */
	LOGIN_FAILED             /* all retries exhausted                       */
} RALoginState;

/* Game load: lifecycle of a game load through rcheevos (authoritative). */
typedef enum {
	GAME_NONE,               /* no game                                     */
	GAME_PENDING_LOGIN,      /* waiting for login to complete               */
	GAME_LOADING,            /* rc_client_begin_identify_and_load_game sent */
	GAME_LOADED,             /* game active, achievements available         */
	GAME_LOAD_RETRY_PENDING  /* load failed offline, retry on connectivity  */
} RAGameState;

/* Sync: lifecycle of the offline-unlock sync engine (derived). */
typedef enum {
	SYNC_IDLE,               /* nothing happening                           */
	SYNC_DEFERRED,           /* waiting for game load before starting       */
	SYNC_RUNNING,            /* sync thread active                          */
	SYNC_ABORTING,           /* abort requested, waiting for thread exit    */
	SYNC_APPLY_PENDING       /* results ready for main thread               */
} RASyncState;

/*****************************************************************************
 * Static state
 *****************************************************************************/

static rc_client_t* ra_client = NULL;

// Authoritative sub-FSM state (SM-4).  These replace the old boolean flags
// (ra_game_loaded, ra_logged_in) — every transition is an explicit enum
// assignment visible in one place.  Forward-declared; enum definitions are
// in the "State machine enums" section below.
static RALoginState ra_login_state = LOGIN_IDLE;
static RAGameState  ra_game_state  = GAME_NONE;

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
	bool active;      // data-validity flag (struct holds valid ROM info)
} RAPendingLoad;

static RAPendingLoad ra_pending_load = {0};

// Login retry state
#define RA_LOGIN_MAX_RETRIES 5
typedef struct {
	int count;
	uint32_t next_time;           // SDL_GetTicks() timestamp for next retry
	bool notified_connecting;     // Track if we showed "Connecting..." notification
} RALoginRetry;

static RALoginRetry ra_login_retry = {0};

// Wifi wait config
#define RA_WIFI_WAIT_MAX_MS 3000   // 3 seconds max blocking wait
#define RA_WIFI_WAIT_POLL_MS 500   // Check every 500ms

// Connectivity probe config
#define RA_PROBE_INTERVAL_MS  30000  // 30 seconds between probe attempts
#define RA_PROBE_SLEEP_CHUNK_MS 200  // Sleep granularity for abort responsiveness

// Lightweight WiFi state polling (detects drops without hitting RA server)
#define RA_WIFI_POLL_INTERVAL_MS 5000  // Check wpa_cli status every 5 seconds

// Connectivity probe state (background thread polls RA login endpoint)
static volatile bool ra_probe_abort = false;
static volatile bool ra_probe_running = false;
static SDL_Thread* ra_probe_thread = NULL;

// Offline sync state (background thread replays pending unlocks)
static SDL_Thread* ra_sync_thread = NULL;
static volatile bool ra_sync_abort = false;
static uint32_t ra_sync_game_id = 0;

// Deferred state: simple main-thread-only flags.  Background threads
// communicate exclusively through the FSM event queue (ra_fsm.h).
// These flags are read and written only on the main thread — no mutex needed.
// Note: game_load_retry is now encoded as ra_game_state == GAME_LOAD_RETRY_PENDING.
static bool ra_deferred_offline_notification = false; /* show "Offline" toast when Notification_init is ready */
static bool ra_deferred_sync_pending = false;         /* sync should start when game is loaded */
static bool ra_user_saw_offline = false;              /* user has seen the "Offline" notification */
static bool ra_sync_apply_pending = false;            /* sync results waiting to be applied */
static uint32_t ra_sync_apply_ids[RA_FSM_MAX_SYNC_IDS];
static uint32_t ra_sync_apply_timestamps[RA_FSM_MAX_SYNC_IDS];
static uint32_t ra_sync_apply_count = 0;

// Probe lifecycle mutex — protects ra_probe_running check-and-set.
// Separate from the FSM event queue mutex.  Held only briefly for the
// atomic read-modify-write in ra_start/stop_connectivity_probe and
// at probe thread exit.  Must NOT be held across SDL_WaitThread.
static SDL_mutex* ra_probe_mutex = NULL;

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
static int ra_response_queue_head = 0;  /* next slot to read  (main thread) */
static int ra_response_queue_tail = 0;  /* next slot to write (worker threads) */
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
static void ra_reset_login_retry(void);
static void ra_start_login(void);
static void ra_start_offline_sync(uint32_t game_id);
static uint32_t ra_get_retry_delay_ms(int attempt);
static void ra_login_callback(int result, const char* error_message, rc_client_t* client, void* userdata);
static void ra_start_connectivity_probe(void);
static void ra_stop_connectivity_probe(void);

// Extracted helpers to reduce duplication
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
 * Helper: Reset login retry context (timer/counter data)
 *****************************************************************************/
static void ra_reset_login_retry(void) {
	ra_login_retry.count = 0;
	ra_login_retry.next_time = 0;
	ra_login_retry.notified_connecting = false;
}

/*****************************************************************************
 * State accessor functions
 *
 * ra_get_conn_state() and ra_get_sync_state() are derived — they compute
 * the current state from underlying flags on each call.
 *
 * ra_get_login_state() and ra_get_game_state() return the authoritative
 * state variable directly (SM-4).
 *
 * Note: ra_get_conn_state() reads ra_probe_running which is protected by
 * ra_probe_mutex, but the accessor is called from the main thread where
 * a momentary stale read is harmless (the flags are re-checked under the
 * lock when an actual transition is made).
 *****************************************************************************/

static RAConnState ra_get_conn_state(void) {
	if (!RA_Offline_isOffline())
		return CONN_ONLINE;
	if (!ra_probe_running)
		return CONN_OFFLINE_NO_PROBE;
	if (ra_probe_abort)
		return CONN_OFFLINE_PROBE_STOPPING;
	return CONN_OFFLINE_PROBING;
}

static RALoginState ra_get_login_state(void) {
	return ra_login_state;
}

static RAGameState ra_get_game_state(void) {
	return ra_game_state;
}

static RASyncState ra_get_sync_state(void) {
	if (RA_Offline_isSyncing()) {
		if (ra_sync_abort)
			return SYNC_ABORTING;
		return SYNC_RUNNING;
	}
	/* sync results waiting for main thread to apply */
	if (ra_sync_apply_pending)
		return SYNC_APPLY_PENDING;
	/* sync trigger deferred — waiting for game load */
	if (ra_deferred_sync_pending)
		return SYNC_DEFERRED;
	return SYNC_IDLE;
}

/* Diagnostic: state → string for logging */
static const char* ra_conn_state_str(RAConnState s) {
	switch (s) {
	case CONN_ONLINE:                return "CONN_ONLINE";
	case CONN_OFFLINE_NO_PROBE:      return "CONN_OFFLINE_NO_PROBE";
	case CONN_OFFLINE_PROBING:       return "CONN_OFFLINE_PROBING";
	case CONN_OFFLINE_PROBE_STOPPING:return "CONN_OFFLINE_PROBE_STOPPING";
	}
	return "CONN_?";
}
static const char* ra_login_state_str(RALoginState s) {
	switch (s) {
	case LOGIN_IDLE:           return "LOGIN_IDLE";
	case LOGIN_IN_PROGRESS:    return "LOGIN_IN_PROGRESS";
	case LOGIN_RETRY_WAITING:  return "LOGIN_RETRY_WAITING";
	case LOGIN_LOGGED_IN:      return "LOGIN_LOGGED_IN";
	case LOGIN_FAILED:         return "LOGIN_FAILED";
	}
	return "LOGIN_?";
}
static const char* ra_game_state_str(RAGameState s) {
	switch (s) {
	case GAME_NONE:               return "GAME_NONE";
	case GAME_PENDING_LOGIN:      return "GAME_PENDING_LOGIN";
	case GAME_LOADING:            return "GAME_LOADING";
	case GAME_LOADED:             return "GAME_LOADED";
	case GAME_LOAD_RETRY_PENDING: return "GAME_LOAD_RETRY_PENDING";
	}
	return "GAME_?";
}
static const char* ra_sync_state_str(RASyncState s) {
	switch (s) {
	case SYNC_IDLE:          return "SYNC_IDLE";
	case SYNC_DEFERRED:      return "SYNC_DEFERRED";
	case SYNC_RUNNING:       return "SYNC_RUNNING";
	case SYNC_ABORTING:      return "SYNC_ABORTING";
	case SYNC_APPLY_PENDING: return "SYNC_APPLY_PENDING";
	}
	return "SYNC_?";
}

/*****************************************************************************
 * Helper: Start a login attempt
 *****************************************************************************/
static void ra_start_login(void) {
	ra_login_state = LOGIN_IN_PROGRESS;
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
	ra_response_queue_head  = 0;
	ra_response_queue_tail  = 0;
	memset(ra_response_queue, 0, sizeof(ra_response_queue));
}

static void ra_queue_quit(void) {
	// Drain any pending responses
	if (ra_queue_mutex) {
		SDL_LockMutex(ra_queue_mutex);
		while (ra_response_queue_count > 0) {
			free(ra_response_queue[ra_response_queue_head].body);
			ra_response_queue[ra_response_queue_head].body = NULL;
			ra_response_queue_head = (ra_response_queue_head + 1) % RA_RESPONSE_QUEUE_SIZE;
			ra_response_queue_count--;
		}
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
		RA_QueuedResponse* resp = &ra_response_queue[ra_response_queue_tail];
		
		// Copy the body data (caller will free original)
		if (body && body_length > 0) {
			resp->body = (char*)malloc(body_length + 1);
			if (resp->body) {
				memcpy(resp->body, body, body_length);
				resp->body[body_length] = '\0';
				resp->body_length = body_length;
			} else {
				resp->body = NULL;
				resp->body_length = 0;
			}
		} else {
			resp->body = NULL;
			resp->body_length = 0;
		}
		
		resp->http_status_code = http_status;
		resp->callback = callback;
		resp->callback_data = callback_data;
		
		ra_response_queue_tail = (ra_response_queue_tail + 1) % RA_RESPONSE_QUEUE_SIZE;
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
		*out = ra_response_queue[ra_response_queue_head];
		ra_response_queue_head = (ra_response_queue_head + 1) % RA_RESPONSE_QUEUE_SIZE;
		ra_response_queue_count--;
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
 * ra_deferred_init / ra_deferred_quit manage the probe mutex and reset
 * main-thread-only flags.  Called from RA_init / RA_quit alongside the
 * response-queue lifecycle.
 *****************************************************************************/

static void ra_deferred_init(void) {
	if (!ra_probe_mutex) {
		ra_probe_mutex = SDL_CreateMutex();
	}
	// Zero all main-thread-only flags (be explicit in case RA_init is
	// called more than once in the process lifetime).
	ra_deferred_offline_notification = false;
	ra_deferred_sync_pending = false;
	ra_user_saw_offline = false;
	ra_sync_apply_pending = false;
	ra_sync_apply_count = 0;
}

static void ra_deferred_quit(void) {
	if (ra_probe_mutex) {
		SDL_DestroyMutex(ra_probe_mutex);
		ra_probe_mutex = NULL;
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
	snprintf(dir_path, sizeof(dir_path), SHARED_USERDATA_PATH "/.ra/muted");
	ra_mkdirs(dir_path);
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
 * Helpers: achievement filtering
 *****************************************************************************/

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
	if (!RA_Offline_ledgerGetPendingByGameHash(game_hash, &pending, &pending_count) ||
	    pending_count == 0 || !pending) {
		return 0;
	}
	
	uint32_t fixed = 0;
	for (uint32_t i = 0; i < pending_count; i++) {
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
	char game_hash[33]; /* Snapshot of ra_game_hash at request time (thread-safe) */
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
		    ra_find_json_bool(body, "Success") == 1) {
				char a_buf[16] = {0};
				if (ra_extract_param(data->post_data, "a", a_buf, sizeof(a_buf))) {
					uint32_t ach_id = (uint32_t)strtoul(a_buf, NULL, 10);
					if (ach_id > 0) {
						RA_LOG_INFO("[AWARD_HTTP] awardachievement SUCCESS for ach=%u\n", ach_id);
						
						// Look up ledger timestamp BEFORE removing from cache.
						// If the ledger still has this entry, use its timestamp.
						// If the ledger was already compacted (sync engine finished),
						// skip the cache patch entirely — the sync engine already
						// patched the cache with the correct timestamp.
						uint32_t unlock_timestamp = 0;  // 0 = "not found"
						bool found_in_ledger = false;
						{
							RA_PendingUnlock entry;
							if (RA_Offline_ledgerFindPendingUnlock(ach_id, &entry)) {
								unlock_timestamp = entry.timestamp;
								found_in_ledger = true;
								RA_LOG_DEBUG("[AWARD_HTTP] ach=%u: found in ledger, "
								            "timestamp=%u\n", ach_id, unlock_timestamp);
							}
						}
						
						RA_Offline_ledgerWriteSyncAck(ach_id, 0);
						RA_Offline_removePendingCacheEntry(ach_id);
						
						if (found_in_ledger && unlock_timestamp > 0) {
							// Ledger entry found — patch the startsession cache
							// with the correct original unlock timestamp.
							RA_LOG_DEBUG("[AWARD_HTTP] ach=%u: patching startsession cache "
							            "with ledger timestamp=%u\n", ach_id, unlock_timestamp);
							RA_Offline_patchStartsessionCacheWithUnlock(
								data->game_hash, ach_id, unlock_timestamp);
						} else {
							// Ledger already compacted (sync engine handled it) OR
							// this was a genuinely online unlock (no ledger entry).
							// Either way, skip cache patching here — the sync engine
							// already patched with the correct timestamp, and using
							// time(NULL) here would corrupt it.
							RA_LOG_DEBUG("[AWARD_HTTP] ach=%u: NOT patching cache "
							            "(found_in_ledger=%d) — sync engine already "
							            "handled or genuinely online unlock\n",
							            ach_id, found_in_ledger);
						}
					}
				}
			}
		}
	} else {
		// Error case — network failure, timeout, DNS error, etc.
		// Extract request type only (don't log full post_data — it may contain API tokens)
		char err_rt_buf[32] = {0};
		const char* err_rtype = ra_extract_param(data->post_data, "r", err_rt_buf, sizeof(err_rt_buf))
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
			ra_extract_param(data->post_data, "m", game_hash, sizeof(game_hash));
			
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
	if (ra_get_conn_state() != CONN_ONLINE) {
		// Extract request type for logging and decision-making
		char rt_buf[32] = {0};
		const char* req_type = ra_extract_param(request->post_data, "r", rt_buf, sizeof(rt_buf))
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
		//
		// "Synthesizable" types (login2, startsession): their response format
		// is simple enough that {"Success":true} is a valid stub. Synthesize
		// a minimal success so rcheevos can proceed.
		//
		// "Non-synthesizable" cacheable types (gameid, achievementsets, patch):
		// these responses contain structured data (GameId, achievement lists,
		// memory maps, etc.) that cannot be faked. A synthetic {"Success":true}
		// causes rcheevos to fail with RC_MISSING_VALUE. Return a retryable
		// error instead — the game load will fail, and we'll retry when
		// connectivity is restored via the game_load_retry deferred flag.
		//
		// Non-cacheable types (awardachievement, ping, submitlbentry, etc.):
		// return a retryable error so rcheevos keeps them in its retry queue.
		bool is_synthesizable = false;
		if (req_type) {
			is_synthesizable = (strcmp(req_type, "login2") == 0 ||
			                    strcmp(req_type, "startsession") == 0);
		}
		
		if (is_synthesizable) {
			// Simple response format — safe to synthesize
			RA_LOG_DEBUG("Offline cache miss (synthesizable): %s — returning synthetic success\n",
			             req_type);
			static const char* empty_success = "{\"Success\":true}";
			if (!ra_queue_push(empty_success, strlen(empty_success), 200, callback, callback_data)) {
				RA_LOG_WARN("Failed to queue synthetic offline response\n");
			}
		} else {
			// Non-synthesizable type — return retryable error.
			// This covers both game-data requests (gameid, achievementsets, patch)
			// whose responses can't be faked, and non-cacheable requests
			// (awardachievement, ping, etc.) that need real server interaction.
			RA_LOG_DEBUG("Offline cache miss (non-synthesizable): %s — returning retryable error\n",
			             req_type ? req_type : "unknown");
			
			// Extra logging for awardachievement in offline mode
			if (req_type && strcmp(req_type, "awardachievement") == 0) {
				char offline_a_buf[16] = {0};
				if (ra_extract_param(request->post_data, "a", offline_a_buf, sizeof(offline_a_buf))) {
					RA_LOG_INFO("[AWARD_GATE] OFFLINE retryable for ach=%s — "
					            "rcheevos will retry when online\n", offline_a_buf);
				}
			}
			
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
	
	// Check if this is an awardachievement request that should be blocked.
	// When the sync engine is handling offline unlocks, rcheevos may also retry
	// its own awardachievement calls (queued during offline mode). These use
	// CLOCK_MONOTONIC for &o= which doesn't advance during device sleep,
	// producing wrong timestamps. Block rcheevos' attempts when:
	//   1. The achievement is still in the pending cache (sync hasn't started it yet), OR
	//   2. The sync engine is actively running (covers the window after pending cache
	//      entry was consumed but before sync finishes and compacts the ledger)
	// In both cases, return a synthetic success so rcheevos clears its retry state.
	char rt_buf[32] = {0};
	const char* req_type = ra_extract_param(request->post_data, "r", rt_buf, sizeof(rt_buf))
	                       ? rt_buf : NULL;
	if (req_type && strcmp(req_type, "awardachievement") == 0) {
		char a_buf[16] = {0};
		if (ra_extract_param(request->post_data, "a", a_buf, sizeof(a_buf))) {
			uint32_t ach_id = (uint32_t)strtoul(a_buf, NULL, 10);
			bool is_pending = (ach_id > 0) && RA_Offline_isUnlockPending(ach_id);
			RASyncState sync = ra_get_sync_state();
			
			RA_LOG_DEBUG("[AWARD_GATE] ra_server_call: awardachievement ach=%u "
			            "is_pending=%d sync=%s\n",
			            ach_id, is_pending, ra_sync_state_str(sync));
			
			if (ach_id > 0 && (is_pending || sync == SYNC_RUNNING || sync == SYNC_ABORTING)) {
				RA_LOG_INFO("[AWARD_GATE] BLOCKED rcheevos award for ach=%u "
				            "(pending=%d, sync=%s) — sync engine handles submission\n",
				            ach_id, is_pending, ra_sync_state_str(sync));
				// Do NOT remove pending cache entry here — that's the sync engine's job.
				// Removing it prematurely would open a window where a subsequent
				// rcheevos retry slips through the is_pending check.
				static const char* synthetic_success = "{\"Success\":true}";
				if (!ra_queue_push(synthetic_success, strlen(synthetic_success), 200, callback, callback_data)) {
					RA_LOG_WARN("[AWARD_GATE] Failed to queue synthetic success for blocked ach=%u\n", ach_id);
				}
				
				// If we blocked because the achievement is pending (not because
				// sync is running), and we still think we're online, then a real
				// HTTP request just failed while the system didn't know WiFi was
				// down.  Transition to offline and start the connectivity probe
				// so that sync fires automatically when WiFi returns.
				if (is_pending && !RA_Offline_isOffline()) {
					RA_LOG_INFO("[AWARD_GATE] HTTP failure detected while "
					            "apparently online — switching to offline mode "
					            "and starting connectivity probe\n");
					RA_Offline_setOffline(true);
					ra_start_connectivity_probe();
				}
				return;
			}
			
			// Not blocked — this is either a genuinely online unlock or a retry
			// after sync completed. Let it through to the real HTTP path.
			RA_LOG_DEBUG("[AWARD_GATE] ALLOWED rcheevos award for ach=%u "
			            "(pending=%d, sync=%s) — sending to server\n",
			            ach_id, is_pending, ra_sync_state_str(sync));
		}
	}
	
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
	/* Snapshot game hash on main thread for safe use in worker callback */
	strncpy(data->game_hash, ra_game_hash, sizeof(data->game_hash) - 1);
	data->game_hash[sizeof(data->game_hash) - 1] = '\0';
	
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
		Notification_push(ra_get_conn_state() != CONN_ONLINE ? NOTIFICATION_OFFLINE_ACHIEVEMENT : NOTIFICATION_ACHIEVEMENT,
		                  message, badge_icon);
		RA_LOG_INFO("[AWARD_TRIGGER] Achievement unlocked: %s (id=%u, points=%d, "
		            "offline=%d, syncing=%d, time_now=%lld)\n",
		       event->achievement->title, event->achievement->id,
		       event->achievement->points,
		       RA_Offline_isOffline(), RA_Offline_isSyncing(),
		       (long long)time(NULL));
		
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
		RA_LOG_WARN("[CONNECTIVITY] DISCONNECTED — switching to offline mode "
		            "(time_now=%lld)\n", (long long)time(NULL));
		RA_Offline_setOffline(true);
		// Force softcore when offline
		rc_client_set_hardcore_enabled(client, 0);
		ra_user_saw_offline = true;
		// Start probe to detect when connectivity returns
		ra_start_connectivity_probe();
		break;
		
	case RC_CLIENT_EVENT_RECONNECTED:
		RA_LOG_INFO("[CONNECTIVITY] RECONNECTED — switching to online mode "
		            "(time_now=%lld)\n", (long long)time(NULL));
		// Stop probe (redundant — rcheevos already confirmed connectivity)
		ra_stop_connectivity_probe();
		RA_Offline_setOffline(false);
		// Re-enable hardcore if configured
		if (CFG_getRAHardcoreMode()) {
			rc_client_set_hardcore_enabled(client, 1);
			RA_LOG_INFO("Hardcore re-enabled after reconnection\n");
			// rc_client_set_hardcore_enabled() sets waiting_for_reset=1 which
			// blocks rc_client_do_frame() from processing any achievements.
			// We must call rc_client_reset() to acknowledge the reset and
			// clear the flag.
			rc_client_reset(client);
			RA_LOG_DEBUG("rc_client_reset complete (cleared waiting_for_reset)\n");
			uint32_t fixed = ra_reapply_pending_unlocks(client, ra_game_hash);
			if (fixed > 0) {
				RA_LOG_INFO("Re-applied %u offline unlock(s) after "
				            "reconnect hardcore re-enable\n", fixed);
			}
		}
		ra_user_saw_offline = false;
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
		// Success — transition to LOGGED_IN
		ra_reset_login_retry();
		ra_login_state = LOGIN_LOGGED_IN;
		RA_LOG_DEBUG("[SM] Login: → %s\n", ra_login_state_str(ra_get_login_state()));
		
		const rc_client_user_t* user = rc_client_get_user_info(client);
		RA_LOG_INFO("Logged in as %s (score: %u)\n",
		       user ? user->display_name : "unknown",
		       user ? user->score : 0);
		
		// Extract internal (server) username from avatar_url.
		// The RA server builds avatar_url from the internal username field
		// (e.g. "/UserPic/SammySwagz.png"), which may differ from
		// display_name if the user has renamed their account.
		if (user && user->avatar_url) {
			CFG_setRAServerUsernameFromAvatarUrl(user->avatar_url);
			if (strlen(CFG_getRAServerUsername()) > 0) {
				RA_LOG_INFO("Extracted server username from avatar_url: '%s' "
				            "(display_name: '%s')\n",
				            CFG_getRAServerUsername(), user->display_name);
			}
		}
		
		// Trigger deferred game load if one is pending
		if (ra_game_state == GAME_PENDING_LOGIN) {
			RA_LOG_DEBUG("Processing deferred game load: %s\n", ra_pending_load.rom_path);
			ra_game_state = GAME_LOADING;
			ra_do_load_game(ra_pending_load.rom_path, ra_pending_load.rom_data, 
			                ra_pending_load.rom_size, ra_pending_load.emu_tag);
			// Don't clear pending game yet — cleared on successful load in
			// ra_game_loaded_callback.  If the load fails (e.g., offline cache
			// miss), the pending info is preserved for retry.
		}
	} else {
		// Failure — schedule retry or give up
		RA_LOG_ERROR("Login failed: %s\n", error_message ? error_message : "unknown error");
		
		if (ra_login_retry.count < RA_LOGIN_MAX_RETRIES) {
			// Schedule retry — transition to RETRY_WAITING
			uint32_t delay = ra_get_retry_delay_ms(ra_login_retry.count);
			ra_login_retry.next_time = SDL_GetTicks() + delay;
			ra_login_retry.count++;
			ra_login_state = LOGIN_RETRY_WAITING;
			
			RA_LOG_DEBUG("[SM] Login: → %s (retry %d/%d in %ums)\n",
			       ra_login_state_str(ra_get_login_state()),
			       ra_login_retry.count, RA_LOGIN_MAX_RETRIES, delay);
			
			// Show "Connecting..." notification on first retry only
			if (ra_login_retry.count == 1 && !ra_login_retry.notified_connecting) {
				ra_login_retry.notified_connecting = true;
				Notification_push(NOTIFICATION_ACHIEVEMENT, 
				                  "Connecting to RetroAchievements...", NULL);
			}
		} else {
			// All retries exhausted — transition to FAILED
			ra_login_state = LOGIN_FAILED;
			RA_LOG_ERROR("All login retries exhausted\n");
			Notification_push(NOTIFICATION_ACHIEVEMENT, 
			                  "RetroAchievements: Connection failed", NULL);
			ra_reset_login_retry();
			if (ra_game_state == GAME_PENDING_LOGIN) {
				ra_game_state = GAME_NONE;
				ra_clear_pending_game();
			}
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
 *
 * State variables (ra_sync_thread, ra_sync_abort, ra_sync_game_id) are
 * declared in the "Static state" section at the top of this file so that
 * ra_get_sync_state() can reference them.
 *****************************************************************************/


/**
 * Background thread function: replay pending offline unlocks using
 * the shared sync engine (ra_sync.c).
 */
static int ra_sync_thread_func(void* data) {
	(void)data;
	
	RA_LOG_INFO("[SYNC_THREAD] Starting sync thread (game_id=%u, time_now=%lld)\n",
	            ra_sync_game_id, (long long)time(NULL));
	
	// Snapshot achievement IDs that are pending BEFORE sync, so we can
	// tell the main thread which ones were synced (after sync, the ledger
	// may have been compacted and the records are gone).
	// Snapshot pending unlocks filtered to the sync game (or all if game_id=0).
	// This serves two purposes: (a) display_count for the notification, and
	// (b) a pre-sync snapshot for post-sync diff to identify which achievements
	// were synced.
	RA_PendingUnlock* pre_unlocks = NULL;
	uint32_t pre_count = 0;
	RA_Offline_ledgerGetPendingByGameId(ra_sync_game_id, &pre_unlocks, &pre_count);
	
	RA_LOG_INFO("[SYNC_THREAD] Pre-sync snapshot: %u pending unlocks (game_id=%u)\n",
	            pre_count, ra_sync_game_id);
	for (uint32_t i = 0; i < pre_count; i++) {
		RA_LOG_INFO("[SYNC_THREAD]   pending[%u]: ach=%u game=%u timestamp=%u hash=%.8s\n",
		            i, pre_unlocks[i].achievement_id, pre_unlocks[i].game_id,
		            pre_unlocks[i].timestamp, pre_unlocks[i].game_hash);
	}
	
	uint32_t display_count = pre_count;

	// Show sync progress in top-left (same area as badge download notifications)
	if (display_count > 0) {
		char msg[NOTIFICATION_MAX_MESSAGE];
		snprintf(msg, sizeof(msg), "Syncing %u offline achievement%s...",
		         display_count, display_count == 1 ? "" : "s");
		Notification_setProgressIndicatorPersistent(true);
		Notification_showProgressIndicator(msg, "", NULL);
	}
	
	// Run the shared sync engine with background timing
	RA_SyncConfig config = RA_SYNC_CONFIG_BACKGROUND;
	RA_SyncResult result = RA_Sync_syncAll(ra_sync_game_id, &config,
	                                       &ra_sync_abort, NULL, NULL);
	
	RA_LOG_INFO("[SYNC_THREAD] Sync complete: %u synced, %u skipped, %u failed (of %u) "
	            "(time_now=%lld)\n",
	            result.synced, result.skipped, result.failed, result.total,
	            (long long)time(NULL));
	
	// Post synced achievement IDs to the FSM event queue for the main thread.
	// We know that RA_Sync_syncAll processes unlocks in order from the
	// pending list, and synced + skipped + failed <= total. The first
	// (synced + skipped) entries in our pre-snapshot were processed.
	// We collect the IDs that match the game filter and were successfully
	// synced so the main thread can update rcheevos unlock bits.
	if (result.synced > 0 && pre_unlocks && pre_count > 0) {
		RAEvent sync_ev;
		memset(&sync_ev, 0, sizeof(sync_ev));
		sync_ev.type = RA_EV_SYNC_DONE;
		
		uint32_t idx = 0;
		uint32_t processed = 0;
		for (uint32_t i = 0; i < pre_count && idx < RA_FSM_MAX_SYNC_IDS; i++) {
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
				sync_ev.data.sync_done.ids[idx] = pre_unlocks[i].achievement_id;
				sync_ev.data.sync_done.timestamps[idx] = pre_unlocks[i].timestamp;
				idx++;
			}
		}
		sync_ev.data.sync_done.count = idx;
		RA_FSM_post(&sync_ev);
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
		RA_FSM_post_signal(RA_EV_SYNC_FAILED);
		ra_start_connectivity_probe();
	}
	
	RA_Offline_setSyncing(false);
	RA_LOG_INFO("[SYNC_THREAD] Sync thread exiting, isSyncing=false (time_now=%lld)\n",
	            (long long)time(NULL));
	return 0;
}

/**
 * Start offline sync if we're online and have pending unlocks.
 * Called after successful online game load or connectivity restoration.
 * 
 * @param game_id  Game to sync (0 = sync all games).
 */
static void ra_start_offline_sync(uint32_t game_id) {
	RAConnState conn = ra_get_conn_state();
	RASyncState sync = ra_get_sync_state();
	RA_LOG_INFO("[SYNC_START] ra_start_offline_sync called: game_id=%u, "
	            "conn=%s, sync=%s, time_now=%lld\n",
	            game_id, ra_conn_state_str(conn), ra_sync_state_str(sync),
	            (long long)time(NULL));
	
	if (conn != CONN_ONLINE) {
		RA_LOG_DEBUG("Sync: skipped (%s)\n", ra_conn_state_str(conn));
		return;
	}
	if (sync == SYNC_RUNNING || sync == SYNC_ABORTING) {
		RA_LOG_DEBUG("Sync: skipped (%s)\n", ra_sync_state_str(sync));
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
	
	/* If a previous sync thread exited but we still hold the handle,
	 * join it to release SDL resources before creating a new one. */
	if (ra_sync_thread) {
		SDL_WaitThread(ra_sync_thread, NULL);
		ra_sync_thread = NULL;
	}
	
	ra_sync_thread = SDL_CreateThread(ra_sync_thread_func, "ra_sync", NULL);
	if (!ra_sync_thread) {
		RA_LOG_ERROR("Failed to create sync thread\n");
		RA_Offline_setSyncing(false);
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
		RA_FSM_post_signal(RA_EV_PROBE_STOPPED);
		SDL_LockMutex(ra_probe_mutex);
		ra_probe_running = false;
		SDL_UnlockMutex(ra_probe_mutex);
		return 1;
	}
	
	bool probe_succeeded = false;
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
			RA_FSM_post_signal(RA_EV_PROBE_STOPPED);
			SDL_LockMutex(ra_probe_mutex);
			ra_probe_running = false;
			SDL_UnlockMutex(ra_probe_mutex);
			return 1;
		}
		
		RA_LOG_DEBUG("Probe: attempting login...\n");
		
		// Synchronous HTTP POST (background thread, blocking OK)
		HTTP_Response* http_resp = HTTP_post(request.url, request.post_data,
		                                     request.content_type);
		
		bool success = false;
		if (http_resp && http_resp->http_status == 200 &&
		    http_resp->data && http_resp->size > 0) {
			// Check for "Success":true in response (handles optional space)
			if (ra_find_json_bool(http_resp->data, "Success") == 1) {
				// Extract server username from AvatarUrl in JSON response.
				CFG_setRAServerUsernameFromAvatarUrl(http_resp->data);
				if (strlen(CFG_getRAServerUsername()) > 0) {
					RA_LOG_INFO("Probe: extracted server username "
					            "from AvatarUrl: '%s'\n", CFG_getRAServerUsername());
				}
				
				// Cache the login response (write-through)
				RA_Offline_cacheResponse(request.url, request.post_data,
				                         http_resp->data, http_resp->size);
				
				// Flip to online mode
				RA_Offline_setOffline(false);
				
				RA_LOG_INFO("[SM] Conn: → CONN_ONLINE (probe success, time_now=%lld)\n",
				            (long long)time(NULL));
				
				// Post event for main thread.  The event payload carries
				// whether hardcore should be re-enabled and whether the
				// offline notification was still pending (so the main
				// thread can decide to suppress both offline + online toasts).
				{
					bool hc_enable = CFG_getRAHardcoreMode();
					RAEvent ev;
					memset(&ev, 0, sizeof(ev));
					ev.type = RA_EV_PROBE_ONLINE;
					ev.data.probe_online.hardcore_enable = hc_enable;
					/* The main thread checks ra_deferred_offline_notification
					 * directly — the probe can't safely read a main-thread-only
					 * flag, so we always set offline_notif_cancel = false here.
					 * The main thread determines if the cancel applies. */
					ev.data.probe_online.offline_notif_cancel = false;
					RA_FSM_post(&ev);
				}
				
				success = true;
				probe_succeeded = true;
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
	// Post stopped event if we didn't already post PROBE_ONLINE
	if (!probe_succeeded) {
		RA_FSM_post_signal(RA_EV_PROBE_STOPPED);
	}
	SDL_LockMutex(ra_probe_mutex);
	ra_probe_running = false;
	SDL_UnlockMutex(ra_probe_mutex);
	return 0;
}

/**
 * Start the connectivity probe thread (if not already running).
 *
 * Called from both the main thread and the sync background thread, so
 * the check-and-set of ra_probe_running is protected by ra_probe_mutex
 * to prevent a TOCTOU race that could launch duplicate probe threads.
 */
static void ra_start_connectivity_probe(void) {
	/* Atomically check-and-set probe state under ra_probe_mutex.
	 * The lock must NOT be held across SDL_WaitThread/SDL_CreateThread
	 * because the probe thread itself locks ra_probe_mutex at exit. */
	SDL_LockMutex(ra_probe_mutex);
	if (ra_probe_running) {
		SDL_UnlockMutex(ra_probe_mutex);
		RA_LOG_DEBUG("Probe: already running, not starting another\n");
		return;
	}
	ra_probe_abort = false;
	ra_probe_running = true;
	SDL_UnlockMutex(ra_probe_mutex);
	
	/* If a previous probe thread exited but we still hold the handle, join it
	 * to release SDL resources before creating a new one. */
	if (ra_probe_thread) {
		SDL_WaitThread(ra_probe_thread, NULL);
		ra_probe_thread = NULL;
	}
	
	ra_probe_thread = SDL_CreateThread(ra_connectivity_probe_func, "ra_probe", NULL);
	if (!ra_probe_thread) {
		RA_LOG_ERROR("Failed to create connectivity probe thread\n");
		SDL_LockMutex(ra_probe_mutex);
		ra_probe_running = false;
		SDL_UnlockMutex(ra_probe_mutex);
		return;
	}
	
	RA_LOG_INFO("Connectivity probe thread launched\n");
}

/**
 * Stop the connectivity probe thread (if running).
 * Joins the thread to ensure it has fully exited before returning.
 * May block up to HTTP_TIMEOUT_SECS (30s) while an in-flight request completes.
 */
static void ra_stop_connectivity_probe(void) {
	if (!ra_probe_thread) return;
	
	RA_LOG_DEBUG("Stopping connectivity probe...\n");
	ra_probe_abort = true;
	
	SDL_WaitThread(ra_probe_thread, NULL);
	ra_probe_thread = NULL;
	SDL_LockMutex(ra_probe_mutex);
	ra_probe_running = false;
	SDL_UnlockMutex(ra_probe_mutex);
	RA_LOG_DEBUG("Connectivity probe thread joined\n");
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
		ra_game_state = GAME_LOADED;
		RA_LOG_DEBUG("[SM] Game: → %s\n", ra_game_state_str(ra_get_game_state()));
		
		// Game loaded successfully — clear pending load info (no longer needed
		// for retry).  Must happen before any early returns below.
		ra_clear_pending_game();
		
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
			Notification_push(NOTIFICATION_ACHIEVEMENT,
			                  "No achievements found for this game", NULL);
		}
	} else {
		RA_LOG_ERROR("Game load failed: %s\n", error_message ? error_message : "unknown error");
		
		// If we're offline, the failure is likely a cache miss for game data
		// (gameid/achievementsets/patch). Schedule a retry when connectivity
		// is restored — don't show "no achievements" since it's transient.
		if (ra_get_conn_state() != CONN_ONLINE) {
			ra_game_state = GAME_LOAD_RETRY_PENDING;
			RA_LOG_INFO("Game load failed while offline — will retry on connectivity restore\n");
		} else {
			// Genuine failure while online — clear pending load data (T7 fix)
			ra_game_state = GAME_NONE;
			ra_clear_pending_game();
			Notification_push(NOTIFICATION_ACHIEVEMENT,
			                  "No achievements found for this game", NULL);
		}
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
	
	// Initialize the FSM event queue (before any threads can post events)
	RA_FSM_init();
	
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
		ra_deferred_offline_notification = true;
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
	if (ra_get_conn_state() != CONN_ONLINE) {
		rc_client_set_hardcore_enabled(ra_client, 0);
	} else {
		rc_client_set_hardcore_enabled(ra_client, CFG_getRAHardcoreMode() ? 1 : 0);
	}
	
	// Reset login/game state before attempting
	ra_login_state = LOGIN_IDLE;
	ra_game_state = GAME_NONE;
	ra_reset_login_retry();
	
	// Attempt login with stored token
	RA_LOG_INFO("Credentials check: authenticated=%d, token_len=%zu, username=%s\n",
	            CFG_getRAAuthenticated(), strlen(CFG_getRAToken()), CFG_getRAUsername());
	if (CFG_getRAAuthenticated() && strlen(CFG_getRAToken()) > 0) {
		RA_LOG_INFO("Logging in with stored token (conn=%s)...\n",
		            ra_conn_state_str(ra_get_conn_state()));
		ra_start_login();
		
		// Launch connectivity probe if we started offline with a cached login
		if (launch_probe) {
			ra_start_connectivity_probe();
		}
	} else {
		RA_LOG_WARN("No stored token - user needs to authenticate in settings\n");
	}
	RA_LOG_INFO("RA_init() complete — conn=%s login=%s game=%s sync=%s\n",
	            ra_conn_state_str(ra_get_conn_state()),
	            ra_login_state_str(ra_get_login_state()),
	            ra_game_state_str(ra_get_game_state()),
	            ra_sync_state_str(ra_get_sync_state()));
}

void RA_quit(void) {
	// Abort connectivity probe (first pass — may be restarted by sync thread)
	ra_stop_connectivity_probe();
	
	// Abort any running sync — join the thread to avoid use-after-free
	if (ra_sync_thread) {
		RA_LOG_INFO("Joining sync thread for shutdown\n");
		ra_sync_abort = true;
		SDL_WaitThread(ra_sync_thread, NULL);
		ra_sync_thread = NULL;
		RA_LOG_DEBUG("Sync thread joined\n");
	}
	
	// Stop probe again in case the sync thread restarted it on failure
	ra_stop_connectivity_probe();
	
	// Shut down the FSM event queue (all threads are joined, drain any leftovers)
	RA_FSM_quit();
	
	// Clear any pending game data
	ra_clear_pending_game();
	
	// Reset login/game state
	ra_login_state = LOGIN_IDLE;
	ra_game_state = GAME_NONE;
	ra_reset_login_retry();
	
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
	RA_LOG_INFO("RA_loadGame: client=%p, RAEnable=%d, login=%s, path=%s\n",
	            (void*)ra_client, CFG_getRAEnable(),
	            ra_login_state_str(ra_get_login_state()), rom_path);
	if (!ra_client || !CFG_getRAEnable()) {
		return;
	}
	
	// If not logged in yet, store the game info for deferred loading
	if (ra_get_login_state() != LOGIN_LOGGED_IN) {
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
		ra_game_state = GAME_PENDING_LOGIN;
		return;
	}
	
	// Already logged in - load immediately
	ra_game_state = GAME_LOADING;
	ra_do_load_game(rom_path, rom_data, rom_size, emu_tag);
}

void RA_unloadGame(void) {
	if (!ra_client) {
		return;
	}
	
	if (ra_game_state == GAME_LOADED) {
		RA_LOG_INFO("Unloading game\n");
		
		// Abort connectivity probe and sync before unloading
		ra_stop_connectivity_probe();
		
		if (ra_get_sync_state() == SYNC_RUNNING) {
			RA_LOG_INFO("Aborting offline sync for game unload\n");
			ra_sync_abort = true;
			// Give sync thread time to notice the abort
			for (int i = 0; i < 10 && ra_get_sync_state() != SYNC_IDLE; i++) {
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
	}
	
	// Clear any pending game data and reset game state.
	// Handles all game states: PENDING_LOGIN, LOADING, LOAD_RETRY_PENDING, etc.
	ra_clear_pending_game();
	ra_game_state = GAME_NONE;
}

/**
 * Named action: handle probe-online event.
 *
 * The connectivity probe confirmed we're back online.  This triggers:
 * - Cancel pending offline notification if user hasn't seen it yet
 * - Otherwise mark the online transition for UI
 * - Re-enable hardcore mode if configured
 * - Trigger sync of pending offline unlocks
 */
static void action_probe_online(const RAEvent* ev) {
	RA_LOG_INFO("[SM] action_probe_online: hardcore=%d\n",
	            ev->data.probe_online.hardcore_enable);
	
	// If the deferred offline notification hasn't been shown yet, cancel it.
	// The user never saw "Offline" so showing "Connected" is meaningless noise.
	if (ra_deferred_offline_notification) {
		ra_deferred_offline_notification = false;
		RA_LOG_INFO("Probe: cancelled pending offline notification "
		            "(connectivity arrived in time)\n");
	} else {
		ra_user_saw_offline = false;
	}
	
	// Re-enable hardcore if configured
	if (ev->data.probe_online.hardcore_enable) {
		if (ra_client && CFG_getRAHardcoreMode()) {
			rc_client_set_hardcore_enabled(ra_client, 1);
			RA_LOG_INFO("Hardcore re-enabled after online transition\n");
			rc_client_reset(ra_client);
			RA_LOG_DEBUG("rc_client_reset complete (cleared waiting_for_reset)\n");
			uint32_t fixed = ra_reapply_pending_unlocks(ra_client, ra_game_hash);
			if (fixed > 0) {
				RA_LOG_INFO("Re-applied %u offline unlock(s) after "
				            "hardcore re-enable\n", fixed);
			}
		}
	}
	
	// Trigger sync — deferred until game is loaded
	ra_deferred_sync_pending = true;
}

/**
 * Named action: handle sync-done event.
 *
 * The sync thread successfully synced achievements with the server.
 * Store the results for application to rcheevos state.
 */
static void action_sync_done(const RAEvent* ev) {
	RA_LOG_INFO("[SM] action_sync_done: count=%u\n", ev->data.sync_done.count);
	
	ra_sync_apply_pending = true;
	ra_sync_apply_count = ev->data.sync_done.count;
	if (ev->data.sync_done.count > 0) {
		memcpy(ra_sync_apply_ids, ev->data.sync_done.ids,
		       ev->data.sync_done.count * sizeof(uint32_t));
		memcpy(ra_sync_apply_timestamps, ev->data.sync_done.timestamps,
		       ev->data.sync_done.count * sizeof(uint32_t));
	}
}

/**
 * Named action: handle sync-failed event.
 *
 * The sync thread encountered failures.  Device goes back to offline mode:
 * - Show offline notification
 * - Disable hardcore
 * - Mark user as having seen offline state
 */
static void action_sync_failed(const RAEvent* ev) {
	(void)ev;
	RA_LOG_WARN("[SM] action_sync_failed: reverting to offline mode\n");
	
	ra_user_saw_offline = true;
	ra_deferred_offline_notification = true;
	
	if (ra_client) {
		rc_client_set_hardcore_enabled(ra_client, 0);
		RA_LOG_INFO("Hardcore disabled after sync failure (offline mode)\n");
	}
}

/**
 * Process FSM events and deferred main-thread state.
 *
 * Called periodically from RA_doFrame() (~every 500ms) and from RA_idle().
 * Drains the FSM event queue (populated by background threads), then
 * processes main-thread-only deferred flags (offline notification,
 * sync pending, sync apply, login retry, game load retry).
 */
static void ra_process_deferred_flags(void) {
	// --- Phase 1: Drain FSM event queue and dispatch to action functions ---
	{
		RAEvent ev_buf[RA_FSM_QUEUE_CAPACITY];
		uint32_t ev_count = RA_FSM_drain(ev_buf, RA_FSM_QUEUE_CAPACITY);
		
		for (uint32_t i = 0; i < ev_count; i++) {
			const RAEvent* ev = &ev_buf[i];
			switch (ev->type) {
			case RA_EV_PROBE_ONLINE:
				action_probe_online(ev);
				break;
			case RA_EV_PROBE_STOPPED:
				RA_LOG_DEBUG("[SM] Probe stopped (abort or failure)\n");
				break;
			case RA_EV_SYNC_DONE:
				action_sync_done(ev);
				break;
			case RA_EV_SYNC_FAILED:
				action_sync_failed(ev);
				break;
			default:
				RA_LOG_WARN("[SM] Unknown event type %d\n", ev->type);
				break;
			}
		}
	}
	
	// --- Phase 2: Process main-thread-only deferred state ---
	
	// Deferred offline notification (set at RA_init or by sync failure)
	if (ra_deferred_offline_notification) {
		ra_user_saw_offline = true;
		ra_deferred_offline_notification = false;
	}
	
	// Deferred sync trigger (set by probe-online action)
	if (ra_deferred_sync_pending) {
		RAGameState gs = ra_get_game_state();
		RA_LOG_INFO("[DEFERRED] sync_pending=true, game=%s, time_now=%lld\n",
		            ra_game_state_str(gs), (long long)time(NULL));
		// Don't start sync until the game is loaded — the sync thread will
		// compact the ledger when done, which removes pending records needed
		// by startsession patching and ra_reapply_pending_unlocks. If we sync
		// before the game loads, those records are gone before rcheevos can
		// use them, causing the just-synced achievement to appear locked.
		if (gs == GAME_LOADED) {
			ra_deferred_sync_pending = false;
			const rc_client_game_t* g = rc_client_get_game_info(ra_client);
			ra_start_offline_sync(g ? g->id : 0);
		}
		// else: stays pending until game loads
	}
	
	// Apply synced achievement unlock state to rcheevos.
	// The sync thread confirmed these achievements with the RA server; now we
	// update rcheevos' internal unlock bits so the achievement list and summary
	// reflect the correct state without needing to restart the game.
	if (ra_sync_apply_pending) {
		// If the game isn't loaded yet, rcheevos doesn't have achievement data
		// so we can't update unlock bits. Keep pending until game loads.
		if (ra_get_game_state() == GAME_LOADED && ra_client) {
			ra_sync_apply_pending = false;
			uint32_t applied = 0;
			uint8_t mode = rc_client_get_hardcore_enabled(ra_client)
				? RC_CLIENT_ACHIEVEMENT_UNLOCKED_BOTH
				: RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE;
			
			for (uint32_t i = 0; i < ra_sync_apply_count; i++) {
				const rc_client_achievement_t* ach =
					rc_client_get_achievement_info(ra_client, ra_sync_apply_ids[i]);
				if (ach && !(ach->unlocked & mode)) {
					// Cast away const — rc_client_get_achievement_info returns a const
					// pointer to the internal struct, but we need to update unlock state.
					// This is safe: the data is mutable and we're on the main thread.
					rc_client_achievement_t* mutable_ach = (rc_client_achievement_t*)ach;
					mutable_ach->unlocked |= mode;
					time_t old_unlock_time = mutable_ach->unlock_time;
					mutable_ach->unlock_time = (time_t)ra_sync_apply_timestamps[i];
					RA_LOG_INFO("Deferred sync-apply: ach %u old_unlock_time=%lld "
					            "new_unlock_time(ledger)=%lld\n",
					            ra_sync_apply_ids[i], (long long)old_unlock_time,
					            (long long)mutable_ach->unlock_time);
					if (mutable_ach->state == RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE) {
						mutable_ach->state = RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
					}
					applied++;
				}
			}
			
			if (applied > 0) {
				RA_LOG_INFO("Applied %u synced achievement unlocks to rcheevos state\n",
				            applied);
			}
		}
		// else: stays pending until game loads
	}
	
	// Check for pending login retry timer
	if (ra_client && ra_login_state == LOGIN_RETRY_WAITING &&
	    SDL_GetTicks() >= ra_login_retry.next_time) {
		ra_start_login();   /* sets ra_login_state = LOGIN_IN_PROGRESS */
	}
	
	// Retry game load after connectivity is restored.
	// The game load may have failed due to an offline cache miss for game data
	// requests (gameid, achievementsets, patch). Now that we're online, retry
	// with the preserved pending load info.
	if (ra_game_state == GAME_LOAD_RETRY_PENDING) {
		RAConnState conn = ra_get_conn_state();
		if (conn == CONN_ONLINE) {
			if (ra_pending_load.active && ra_client) {
				ra_game_state = GAME_LOADING;
				RA_LOG_INFO("Retrying game load after connectivity restored: %s\n",
				            ra_pending_load.rom_path);
				ra_do_load_game(ra_pending_load.rom_path, ra_pending_load.rom_data,
				                ra_pending_load.rom_size, ra_pending_load.emu_tag);
				// ra_clear_pending_game() is called in ra_game_loaded_callback on success.
				// If it fails again (unlikely now that we're online), the callback
				// won't set GAME_LOAD_RETRY_PENDING again (only set when offline).
			} else {
				ra_game_state = GAME_NONE;
				RA_LOG_WARN("GAME_LOAD_RETRY_PENDING but no pending load or no client\n");
			}
		}
		// else: stays pending until online
	}
	
	// --- Phase 3: Lightweight WiFi connectivity polling ---
	// Check wpa_cli status every RA_WIFI_POLL_INTERVAL_MS to detect WiFi
	// drops/restores without hitting the RA server.  This catches the case
	// where WiFi disappears mid-game before any HTTP request fails (e.g.,
	// router reboot, wifi_init.sh toggle, moving out of range).
	{
		static uint32_t last_wifi_poll = 0;
		uint32_t now = SDL_GetTicks();
		if (ra_client && now - last_wifi_poll >= RA_WIFI_POLL_INTERVAL_MS) {
			last_wifi_poll = now;
			bool wifi_up = PLAT_wifiConnected();
			
			if (!wifi_up && !RA_Offline_isOffline()) {
				// WiFi dropped while we thought we were online
				RA_LOG_WARN("[WIFI_POLL] WiFi connection lost — "
				            "switching to offline mode\n");
				RA_Offline_setOffline(true);
				ra_user_saw_offline = true;
				ra_start_connectivity_probe();
			} else if (wifi_up && RA_Offline_isOffline() &&
			           !ra_probe_running) {
				// WiFi is back but no probe is running (edge case:
				// probe was never started, or exited without success).
				// Kick off a probe to verify real connectivity and
				// trigger sync.
				RA_LOG_INFO("[WIFI_POLL] WiFi restored while offline, "
				            "no probe running — starting probe\n");
				ra_start_connectivity_probe();
			}
		}
	}
}

void RA_doFrame(void) {
	// Process any pending HTTP responses before checking achievements
	// This ensures game load completes and achievements are active
	ra_process_queued_responses();
	
	if (ra_client && ra_game_state == GAME_LOADED) {
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
	return ra_game_state == GAME_LOADED;
}

bool RA_isHardcoreModeActive(void) {
	if (!ra_client || ra_game_state != GAME_LOADED) {
		return false;
	}
	return rc_client_get_hardcore_enabled(ra_client) != 0;
}

bool RA_isLoggedIn(void) {
	return ra_login_state == LOGIN_LOGGED_IN;
}

const char* RA_getUserDisplayName(void) {
	if (!ra_client || ra_login_state != LOGIN_LOGGED_IN) {
		return NULL;
	}
	const rc_client_user_t* user = rc_client_get_user_info(ra_client);
	return user ? user->display_name : NULL;
}

const char* RA_getGameTitle(void) {
	if (!ra_client || ra_game_state != GAME_LOADED) {
		return NULL;
	}
	const rc_client_game_t* game = rc_client_get_game_info(ra_client);
	return game ? game->title : NULL;
}

void RA_getAchievementSummary(uint32_t* unlocked, uint32_t* total) {
	if (!ra_client || ra_game_state != GAME_LOADED) {
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
	if (!ra_client || ra_game_state != GAME_LOADED) {
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
	if (ra_game_state != GAME_LOADED || ra_game_hash[0] == '\0') {
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

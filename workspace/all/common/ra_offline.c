#include "ra_offline.h"
#include "sha256.h"
#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* Logging macros using NextUI's LOG_* infrastructure */
#define OFFLINE_LOG_DEBUG(fmt, ...) LOG_debug("[RA_OFFLINE] " fmt, ##__VA_ARGS__)
#define OFFLINE_LOG_INFO(fmt, ...)  LOG_info("[RA_OFFLINE] " fmt, ##__VA_ARGS__)
#define OFFLINE_LOG_WARN(fmt, ...)  LOG_warn("[RA_OFFLINE] " fmt, ##__VA_ARGS__)
#define OFFLINE_LOG_ERROR(fmt, ...) LOG_error("[RA_OFFLINE] " fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Static state
 *****************************************************************************/

static bool ra_offline_initialized = false;
static volatile bool ra_offline_mode = false;
static volatile bool ra_sync_in_progress = false;

/* Mutex protecting hash-chain state, pending cache, and the write queue.
 * Guards: ledger_append (hash chain + enqueue), ledgerCompact,
 *         ledgerGetPendingUnlocks, and all pending-cache functions. */
static SDL_mutex* ra_ledger_mutex = NULL;

/* Base data directory (set during init) */
static char ra_data_dir[512] = {0};
static char ra_cache_dir[512] = {0};
static char ra_ledger_path[512] = {0};

/* Running hash chain state: SHA-256 of the last ledger record written */
static uint8_t ra_ledger_last_hash[SHA256_DIGEST_SIZE];
static bool ra_ledger_has_records = false;

/* Cached pending offline achievement IDs for quick lookup */
#define RA_MAX_PENDING_CACHE 512
static uint32_t ra_pending_ids[RA_MAX_PENDING_CACHE];
static uint32_t ra_pending_count = 0;

/*****************************************************************************
 * Async ledger write queue
 *
 * Ledger writes (fopen/fwrite/fsync/fclose) are handed off to a background
 * thread so that the main game loop is never stalled by SD-card I/O.
 *
 * The hash chain (prev_hash / record_hash computation) is still finalized
 * synchronously under ra_ledger_mutex before the record is enqueued — this
 * preserves strict ordering without blocking on disk.
 *****************************************************************************/

#define RA_LEDGER_WRITE_QUEUE_SIZE 32  /* more than enough for any burst */

typedef struct {
	RA_LedgerRecord records[RA_LEDGER_WRITE_QUEUE_SIZE];
	int head;         /* next slot to read  (writer thread) */
	int tail;         /* next slot to write (producers)     */
	int count;
	SDL_mutex*  mutex;
	SDL_cond*   cond_nonempty;  /* signalled when a record is enqueued */
	SDL_cond*   cond_empty;     /* signalled when queue drains to zero  */
	SDL_Thread* thread;
	volatile bool running;
} LedgerWriteQueue;

static LedgerWriteQueue ra_ledger_wq = {0};

/*****************************************************************************
 * Helpers: Directory creation
 *****************************************************************************/

static int mkdirs(const char* path) {
	char tmp[512];
	char* p;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/') {
		tmp[len - 1] = '\0';
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

/*****************************************************************************
 * Helpers: URL parameter extraction
 *
 * rcheevos URLs use query parameters like ?r=login2, ?r=patch, etc.
 * POST data also contains parameters like r=awardachievement&u=...
 *****************************************************************************/

/**
 * Extract the value of parameter 'key' from a URL query string or POST body.
 * Writes into buf (max buf_size). Returns true if found.
 */
static bool extract_param(const char* str, const char* key, char* buf, size_t buf_size) {
	if (!str || !key || !buf || buf_size == 0) return false;

	size_t key_len = strlen(key);
	const char* p = str;

	while ((p = strstr(p, key)) != NULL) {
		/* Ensure this is a parameter start (beginning of string, after ? or &) */
		if (p != str) {
			char before = *(p - 1);
			if (before != '?' && before != '&') {
				p += key_len;
				continue;
			}
		}
		/* Check for '=' after key */
		if (p[key_len] == '=') {
			const char* val = p + key_len + 1;
			const char* end = val;
			while (*end && *end != '&' && *end != '#' && *end != ' ') end++;
			size_t vlen = (size_t)(end - val);
			if (vlen >= buf_size) vlen = buf_size - 1;
			memcpy(buf, val, vlen);
			buf[vlen] = '\0';
			return true;
		}
		p += key_len;
	}
	return false;
}

/**
 * Get the request type from URL or post_data.
 * Returns pointer to static string or NULL.
 */
static const char* get_request_type(const char* url, const char* post_data,
                                    char* buf, size_t buf_size) {
	/* Try URL first (GET requests have ?r=... in URL) */
	if (url && extract_param(url, "r", buf, buf_size)) {
		return buf;
	}
	/* Try POST data */
	if (post_data && extract_param(post_data, "r", buf, buf_size)) {
		return buf;
	}
	return NULL;
}

/**
 * Get the game hash from post_data (used for cache key).
 */
static bool get_game_hash_param(const char* url, const char* post_data, char* buf, size_t buf_size) {
	/* rcheevos sends game hash as 'm' parameter in some requests, or 'h' */
	if (post_data) {
		if (extract_param(post_data, "m", buf, buf_size)) return true;
		if (extract_param(post_data, "h", buf, buf_size)) return true;
	}
	if (url) {
		if (extract_param(url, "m", buf, buf_size)) return true;
		if (extract_param(url, "h", buf, buf_size)) return true;
	}
	return false;
}

/*****************************************************************************
 * Response cache implementation
 *
 * Cache file format:
 *   [uint32_t body_len] [body_bytes...] [32-byte SHA-256 of body]
 *
 * We cache:
 *   - login2 responses  -> cache/login.bin
 *   - patch responses    -> cache/<game_hash>.bin
 *****************************************************************************/

static void cache_file_path(const char* request_type, const char* url,
                            const char* post_data, char* buf, size_t buf_size) {
	if (strcmp(request_type, "login2") == 0) {
		snprintf(buf, buf_size, "%s/login.bin", ra_cache_dir);
	} else if (strcmp(request_type, "achievementsets") == 0 ||
	           strcmp(request_type, "gameid") == 0 ||
	           strcmp(request_type, "patch") == 0 ||
	           strcmp(request_type, "startsession") == 0) {
		char hash[64] = {0};
		if (get_game_hash_param(url, post_data, hash, sizeof(hash))) {
			snprintf(buf, buf_size, "%s/%s_%s.bin", ra_cache_dir, request_type, hash);
		} else {
			/* Fallback: use request type only */
			snprintf(buf, buf_size, "%s/%s.bin", ra_cache_dir, request_type);
		}
	} else {
		buf[0] = '\0';
	}
}

static bool is_cacheable_request(const char* request_type) {
	return request_type &&
	       (strcmp(request_type, "login2") == 0 ||
	        strcmp(request_type, "achievementsets") == 0 ||
	        strcmp(request_type, "gameid") == 0 ||
	        strcmp(request_type, "patch") == 0 ||
	        strcmp(request_type, "startsession") == 0);
}

void RA_Offline_cacheResponse(const char* url, const char* post_data,
                              const char* response_body, size_t response_len) {
	if (!ra_offline_initialized || !response_body || response_len == 0) return;

	char type_buf[32];
	const char* req_type = get_request_type(url, post_data, type_buf, sizeof(type_buf));
	if (!is_cacheable_request(req_type)) return;

	char path[512];
	cache_file_path(req_type, url, post_data, path, sizeof(path));
	if (path[0] == '\0') return;

	/* Compute SHA-256 of body */
	uint8_t digest[SHA256_DIGEST_SIZE];
	sha256_hash(response_body, response_len, digest);

	FILE* f = fopen(path, "wb");
	if (!f) {
		OFFLINE_LOG_ERROR("Failed to open cache file for writing: %s (%s)\n", path, strerror(errno));
		return;
	}

	uint32_t len32 = (uint32_t)response_len;
	size_t written = 0;
	written += fwrite(&len32, sizeof(len32), 1, f);
	written += fwrite(response_body, 1, response_len, f) > 0 ? 1 : 0;
	written += fwrite(digest, SHA256_DIGEST_SIZE, 1, f);

	if (written < 3) {
		OFFLINE_LOG_ERROR("Failed to write cache file: %s\n", path);
		fclose(f);
		unlink(path);
		return;
	}

	fflush(f);
	fsync(fileno(f));
	fclose(f);

	OFFLINE_LOG_DEBUG("Cached %s response (%u bytes) to %s\n", req_type, len32, path);
}

/**
 * Patch a cached startsession JSON response with offline ledger unlocks.
 *
 * The startsession response contains "Unlocks":[...] and "HardcoreUnlocks":[...]
 * arrays that tell rcheevos which achievements are already unlocked. When we have
 * offline ledger entries for achievements unlocked after the cache was written,
 * we inject them into BOTH arrays so rcheevos shows them as unlocked in both
 * softcore and hardcore modes.  This prevents rcheevos from re-triggering and
 * re-awarding them through its own server call (which would lack the &o=
 * seconds_since parameter, causing the RA server to record the sync time
 * instead of the original unlock time).
 *
 * @param body        The cached JSON body (will be freed and replaced)
 * @param body_len    Length of body
 * @param game_hash   Game hash to filter ledger entries by
 * @param out_body    Output: patched body (caller must free)
 * @param out_len     Output: patched body length
 * @return true if patching succeeded (or no patching needed), false on error
 */

/**
 * Helper: inject pending unlocks into a single JSON array within the body.
 *
 * Finds the array identified by @p array_key (e.g. "\"Unlocks\""), checks for
 * duplicates, and splices new entries just before the closing ']'.
 *
 * @param body           Current body buffer (NOT freed — caller manages memory)
 * @param body_len       Length of body
 * @param array_key      JSON key string to locate, e.g. "\"Unlocks\""
 * @param pending        Array of pending unlocks (already filtered to this game)
 * @param pending_count  Number of entries in pending array
 * @param game_hash      Game hash to filter by
 * @param out_body       Output: new body (malloc'd) or NULL if no injection needed
 * @param out_len        Output: new body length
 * @return number of entries injected (0 means no change, out_body is NULL)
 */
static uint32_t inject_unlocks_into_array(const char* body, size_t body_len,
                                          const char* array_key,
                                          const RA_PendingUnlock* pending,
                                          uint32_t pending_count,
                                          const char* game_hash,
                                          char** out_body, size_t* out_len) {
	*out_body = NULL;
	*out_len = 0;

	const char* arr_key_pos = strstr(body, array_key);
	if (!arr_key_pos) {
		return 0;
	}

	/* Find the '[' after the key */
	const char* arr_start = strchr(arr_key_pos + strlen(array_key), '[');
	if (!arr_start)
		return 0;

	/* Find the matching ']' */
	const char* arr_end = strchr(arr_start, ']');
	if (!arr_end)
		return 0;

	/* Check which pending achievement IDs need injection (not already present) */
	bool* need_inject = (bool*)calloc(pending_count, sizeof(bool));
	if (!need_inject)
		return 0;

	uint32_t inject_count = 0;
	for (uint32_t i = 0; i < pending_count; i++) {
		/* When game_hash is provided, only inject entries for that game.
		 * When game_hash is NULL, accept all entries (used by single-entry
		 * callers like patchStartsessionCacheWithUnlock). */
		if (game_hash && strcmp(pending[i].game_hash, game_hash) != 0)
			continue;

		char id_pattern[32];
		snprintf(id_pattern, sizeof(id_pattern), "\"ID\":%u", pending[i].achievement_id);
		bool found = false;
		for (const char* p = arr_start; p < arr_end; p++) {
			if (strncmp(p, id_pattern, strlen(id_pattern)) == 0) {
				found = true;
				break;
			}
		}
		if (!found) {
			need_inject[i] = true;
			inject_count++;
		}
	}

	if (inject_count == 0) {
		free(need_inject);
		return 0;
	}

	/* Log each achievement being injected */
	for (uint32_t i = 0; i < pending_count; i++) {
		if (!need_inject[i]) continue;
		OFFLINE_LOG_INFO("inject_unlocks: %s ach=%u timestamp=%u\n",
		                 array_key, pending[i].achievement_id,
		                 pending[i].timestamp);
	}

	/* Build the injection string */
	size_t inject_buf_size = inject_count * 48 + 1;
	char* inject_buf = (char*)malloc(inject_buf_size);
	if (!inject_buf) {
		free(need_inject);
		return 0;
	}

	size_t inject_len = 0;
	bool existing_entries = (arr_end - arr_start > 1);
	for (uint32_t i = 0; i < pending_count; i++) {
		if (!need_inject[i]) continue;
		int written;
		if (existing_entries || inject_len > 0) {
			written = snprintf(inject_buf + inject_len, inject_buf_size - inject_len,
			                   ",{\"ID\":%u,\"When\":%u}",
			                   pending[i].achievement_id, pending[i].timestamp);
		} else {
			written = snprintf(inject_buf + inject_len, inject_buf_size - inject_len,
			                   "{\"ID\":%u,\"When\":%u}",
			                   pending[i].achievement_id, pending[i].timestamp);
		}
		if (written > 0) inject_len += (size_t)written;
	}
	free(need_inject);

	/* Build new body: prefix + inject + suffix */
	size_t prefix_len = (size_t)(arr_end - body);
	size_t suffix_len = body_len - prefix_len;
	size_t new_len = prefix_len + inject_len + suffix_len;
	char* new_body = (char*)malloc(new_len + 1);
	if (!new_body) {
		free(inject_buf);
		return 0;
	}

	memcpy(new_body, body, prefix_len);
	memcpy(new_body + prefix_len, inject_buf, inject_len);
	memcpy(new_body + prefix_len + inject_len, arr_end, suffix_len);
	new_body[new_len] = '\0';
	free(inject_buf);

	*out_body = new_body;
	*out_len = new_len;
	return inject_count;
}

static bool patch_startsession_with_ledger(char* body, size_t body_len,
                                           const char* game_hash,
                                           char** out_body, size_t* out_len) {
	/* Get pending unlocks from ledger */
	RA_PendingUnlock* pending = NULL;
	uint32_t pending_count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&pending, &pending_count))
		goto passthrough;

	if (pending_count == 0 || !pending)
		goto passthrough;

	/* Filter to only unlocks for this game hash */
	uint32_t game_count = 0;
	for (uint32_t i = 0; i < pending_count; i++) {
		if (game_hash && strcmp(pending[i].game_hash, game_hash) == 0) {
			game_count++;
		}
	}

	if (game_count == 0)
		goto passthrough;

	/*
	 * Inject into both "Unlocks" (softcore) and "HardcoreUnlocks" arrays.
	 * This ensures rcheevos sees pending achievements as already unlocked
	 * in both modes, preventing it from re-awarding them via its own
	 * server call (which would lack &o= and record the wrong timestamp).
	 */
	char* current_body = body;
	size_t current_len = body_len;
	bool body_replaced = false;
	uint32_t total_injected = 0;

	/* Pass 1: Inject into "Unlocks" (softcore) */
	{
		char* new_body = NULL;
		size_t new_len = 0;
		uint32_t injected = inject_unlocks_into_array(
			current_body, current_len, "\"Unlocks\"",
			pending, pending_count, game_hash,
			&new_body, &new_len);
		if (injected > 0 && new_body) {
			OFFLINE_LOG_DEBUG("Patching startsession: injected %u ledger unlocks "
			                 "into Unlocks array\n", injected);
			if (body_replaced) {
				free(current_body);
			}
			current_body = new_body;
			current_len = new_len;
			body_replaced = true;
			total_injected += injected;
		}
	}

	/* Pass 2: Inject into "HardcoreUnlocks" */
	{
		char* new_body = NULL;
		size_t new_len = 0;
		uint32_t injected = inject_unlocks_into_array(
			current_body, current_len, "\"HardcoreUnlocks\"",
			pending, pending_count, game_hash,
			&new_body, &new_len);
		if (injected > 0 && new_body) {
			OFFLINE_LOG_DEBUG("Patching startsession: injected %u ledger unlocks "
			                 "into HardcoreUnlocks array\n", injected);
			if (body_replaced) {
				free(current_body);
			}
			current_body = new_body;
			current_len = new_len;
			body_replaced = true;
			total_injected += injected;
		}
	}

	free(pending);

	if (total_injected > 0 && body_replaced) {
		OFFLINE_LOG_DEBUG("Patched startsession: %zu -> %zu bytes\n",
		                 body_len, current_len);
		free(body);
		*out_body = current_body;
		*out_len = current_len;
		return true;
	}

	/* No injections needed — fall through to passthrough */
	*out_body = body;
	*out_len = body_len;
	return true;

passthrough:
	free(pending);
	*out_body = body;
	*out_len = body_len;
	return true;
}

bool RA_Offline_patchStartsessionResponse(char* body, size_t body_len,
                                          const char* game_hash,
                                          char** out_body, size_t* out_len) {
	if (!ra_offline_initialized || !body || body_len == 0) return false;

	/* Delegate to the internal patching function, which reads the ledger
	 * once internally. No need to pre-check — the function is a no-op
	 * when there are no matching pending unlocks. */
	char* orig_body = body;
	if (!patch_startsession_with_ledger(body, body_len, game_hash, out_body, out_len)) {
		return false;
	}

	/* patch_startsession_with_ledger returns the original body unchanged
	 * when no patching is needed. Check if it actually changed. */
	return (*out_body != orig_body);
}

bool RA_Offline_getCachedResponse(const char* url, const char* post_data,
                                  char** out_body, size_t* out_len) {
	if (!ra_offline_initialized || !out_body || !out_len) return false;

	char type_buf[32];
	const char* req_type = get_request_type(url, post_data, type_buf, sizeof(type_buf));
	if (!req_type) return false;

	char path[512];
	cache_file_path(req_type, url, post_data, path, sizeof(path));
	if (path[0] == '\0') return false;

	FILE* f = fopen(path, "rb");
	if (!f) {
		OFFLINE_LOG_DEBUG("Cache miss for %s: %s\n", req_type, path);
		return false;
	}

	/* Read length */
	uint32_t len32;
	if (fread(&len32, sizeof(len32), 1, f) != 1) {
		OFFLINE_LOG_WARN("Cache file corrupt (no length): %s\n", path);
		fclose(f);
		return false;
	}

	/* Sanity check (max 8MB, same as HTTP_MAX_RESPONSE_SIZE) */
	if (len32 > 8 * 1024 * 1024) {
		OFFLINE_LOG_WARN("Cache file body too large (%u): %s\n", len32, path);
		fclose(f);
		return false;
	}

	/* Read body */
	char* body = (char*)malloc(len32 + 1);
	if (!body) {
		OFFLINE_LOG_ERROR("Failed to allocate %u bytes for cache read\n", len32);
		fclose(f);
		return false;
	}
	if (fread(body, 1, len32, f) != len32) {
		OFFLINE_LOG_WARN("Cache file truncated: %s\n", path);
		free(body);
		fclose(f);
		return false;
	}
	body[len32] = '\0';

	/* Read and verify SHA-256 */
	uint8_t stored_digest[SHA256_DIGEST_SIZE];
	if (fread(stored_digest, SHA256_DIGEST_SIZE, 1, f) != 1) {
		OFFLINE_LOG_WARN("Cache file missing digest: %s\n", path);
		free(body);
		fclose(f);
		return false;
	}
	fclose(f);

	uint8_t computed_digest[SHA256_DIGEST_SIZE];
	sha256_hash(body, len32, computed_digest);
	if (memcmp(stored_digest, computed_digest, SHA256_DIGEST_SIZE) != 0) {
		OFFLINE_LOG_WARN("Cache file SHA-256 mismatch (corrupt): %s\n", path);
		free(body);
		unlink(path);  /* Remove corrupt cache */
		return false;
	}

	*out_body = body;
	*out_len = len32;
	OFFLINE_LOG_DEBUG("Cache hit for %s (%u bytes) from %s\n", req_type, len32, path);

	/* For startsession responses served from cache, patch in any pending ledger unlocks
	 * so rcheevos shows offline-earned achievements as unlocked on restart.
	 * Note: we intentionally do NOT gate on ra_offline_mode here — by the time this
	 * function runs, the connectivity probe may have already flipped the flag to false
	 * on its thread, but we still need to patch the cached response.  The underlying
	 * patch_startsession_with_ledger() safely no-ops when there are no pending unlocks. */
	if (strcmp(req_type, "startsession") == 0) {
		char game_hash[64] = {0};
		get_game_hash_param(url, post_data, game_hash, sizeof(game_hash));

		char* pre_patch_body = *out_body;
		if (!patch_startsession_with_ledger(*out_body, *out_len, game_hash,
		                                    out_body, out_len)) {
			OFFLINE_LOG_WARN("Failed to patch startsession with ledger data\n");
		} else if (*out_body != pre_patch_body) {
			OFFLINE_LOG_INFO("Patched offline startsession (%u -> %u bytes)\n",
			                 len32, (unsigned)*out_len);
		}
	}

	return true;
}

/*****************************************************************************
 * Ledger implementation
 *****************************************************************************/

/**
 * Compute the SHA-256 of a complete ledger record (for chain linking).
 * This hashes the entire record including its record_hash field.
 */
static void ledger_hash_record(const RA_LedgerRecord* rec, uint8_t out[SHA256_DIGEST_SIZE]) {
	sha256_hash(rec, sizeof(RA_LedgerRecord), out);
}

/**
 * Compute the record_hash field (hashes everything except record_hash itself).
 */
static void ledger_compute_record_hash(RA_LedgerRecord* rec) {
	sha256_hash(rec, RA_LEDGER_RECORD_HASHABLE_SIZE, rec->record_hash);
}

/**
 * Validate the ledger file and rebuild chain state.
 * Returns the number of valid records, and sets ra_ledger_last_hash.
 */
static uint32_t ledger_validate_and_load(void) {
	FILE* f = fopen(ra_ledger_path, "rb");
	if (!f) {
		OFFLINE_LOG_DEBUG("No ledger file found (will create on first write)\n");
		memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);
		ra_ledger_has_records = false;
		return 0;
	}

	uint32_t valid_count = 0;
	uint32_t total_read = 0;
	bool chain_broken = false;
	uint8_t prev_hash[SHA256_DIGEST_SIZE];
	memset(prev_hash, 0, SHA256_DIGEST_SIZE);

	RA_LedgerRecord rec;
	while (fread(&rec, sizeof(rec), 1, f) == 1) {
		total_read++;

		/* Verify prev_hash chain */
		if (memcmp(rec.prev_hash, prev_hash, SHA256_DIGEST_SIZE) != 0) {
			OFFLINE_LOG_WARN("Ledger chain broken at record %u (skipping)\n", total_read - 1);
			chain_broken = true;
			/* Reset chain expectation to this record's actual prev_hash
			 * so we can continue validating subsequent records */
		}

		/* Verify record_hash (self-integrity, independent of chain) */
		uint8_t expected_hash[SHA256_DIGEST_SIZE];
		sha256_hash(&rec, RA_LEDGER_RECORD_HASHABLE_SIZE, expected_hash);
		if (memcmp(rec.record_hash, expected_hash, SHA256_DIGEST_SIZE) != 0) {
			OFFLINE_LOG_WARN("Ledger record_hash invalid at record %u (skipping)\n", total_read - 1);
			chain_broken = true;
			/* Skip this record but continue reading — don't update prev_hash
			 * so the next record's chain link will also break, but we'll
			 * still read its data for pending-unlock purposes */
			continue;
		}

		/* Record has valid self-hash — update chain state */
		ledger_hash_record(&rec, prev_hash);
		valid_count++;
	}

	fclose(f);

	if (chain_broken) {
		/* Chain was broken but we recovered what we could. Compact to fix.
		 * Don't truncate — that loses valid records after the break point.
		 * Instead, trigger a compaction on next sync to rebuild the chain. */
		OFFLINE_LOG_WARN("Ledger has chain integrity issues (%u/%u records valid). "
		                 "Will be repaired on next compaction.\n", valid_count, total_read);
	}

	memcpy(ra_ledger_last_hash, prev_hash, SHA256_DIGEST_SIZE);
	ra_ledger_has_records = (valid_count > 0);

	OFFLINE_LOG_DEBUG("Ledger loaded: %u valid records\n", valid_count);
	return valid_count;
}

/**
 * Background thread: drains the ledger write queue, writing each record to
 * disk with fsync so data survives a crash.  All blocking I/O happens here,
 * never on the main thread.
 */
static int ledger_writer_thread(void* userdata) {
	(void)userdata;
	LedgerWriteQueue* wq = &ra_ledger_wq;

	while (1) {
		SDL_LockMutex(wq->mutex);

		/* Wait until there is work to do or we're asked to stop */
		while (wq->count == 0 && wq->running) {
			SDL_CondWait(wq->cond_nonempty, wq->mutex);
		}

		if (wq->count == 0 && !wq->running) {
			/* Shutdown signal and queue is empty — exit cleanly */
			SDL_UnlockMutex(wq->mutex);
			break;
		}

		/* Dequeue one record */
		RA_LedgerRecord rec = wq->records[wq->head];
		wq->head = (wq->head + 1) % RA_LEDGER_WRITE_QUEUE_SIZE;
		wq->count--;

		SDL_UnlockMutex(wq->mutex);

		/* Write to disk — blocking I/O, safe on background thread */
		FILE* f = fopen(ra_ledger_path, "ab");
		if (!f) {
			OFFLINE_LOG_ERROR("Ledger writer: failed to open ledger: %s\n",
			                  strerror(errno));
		} else {
			if (fwrite(&rec, sizeof(RA_LedgerRecord), 1, f) != 1) {
				OFFLINE_LOG_ERROR("Ledger writer: fwrite failed: %s\n",
				                  strerror(errno));
			}
			fflush(f);
			fsync(fileno(f));
			fclose(f);
		}

		/* Signal any thread waiting for the queue to drain (e.g. shutdown) */
		SDL_LockMutex(wq->mutex);
		if (wq->count == 0) {
			SDL_CondSignal(wq->cond_empty);
		}
		SDL_UnlockMutex(wq->mutex);
	}

	return 0;
}

/**
 * Start the ledger background writer thread.
 */
static void ledger_writer_start(void) {
	LedgerWriteQueue* wq = &ra_ledger_wq;
	memset(wq, 0, sizeof(*wq));
	wq->mutex         = SDL_CreateMutex();
	wq->cond_nonempty = SDL_CreateCond();
	wq->cond_empty    = SDL_CreateCond();
	wq->running       = true;
	wq->thread        = SDL_CreateThread(ledger_writer_thread, "ra_ledger_writer", NULL);
	if (!wq->thread) {
		OFFLINE_LOG_ERROR("Failed to create ledger writer thread: %s\n", SDL_GetError());
	}
}

/**
 * Stop the ledger background writer thread, flushing any queued records first.
 */
static void ledger_writer_stop(void) {
	LedgerWriteQueue* wq = &ra_ledger_wq;
	if (!wq->thread) return;

	/* Wait for queue to drain, then signal shutdown */
	SDL_LockMutex(wq->mutex);
	while (wq->count > 0) {
		SDL_CondWait(wq->cond_empty, wq->mutex);
	}
	wq->running = false;
	SDL_CondSignal(wq->cond_nonempty);  /* wake thread so it can exit */
	SDL_UnlockMutex(wq->mutex);

	SDL_WaitThread(wq->thread, NULL);
	wq->thread = NULL;

	SDL_DestroyCond(wq->cond_empty);
	SDL_DestroyCond(wq->cond_nonempty);
	SDL_DestroyMutex(wq->mutex);
	wq->mutex         = NULL;
	wq->cond_empty    = NULL;
	wq->cond_nonempty = NULL;
}

/**
 * Append a record to the ledger.
 *
 * The hash chain is finalized synchronously under ra_ledger_mutex (preserving
 * strict ordering), then the completed record is handed off to the background
 * writer thread.  The main thread never blocks on disk I/O.
 */
static bool ledger_append(RA_LedgerRecord* rec) {
	SDL_LockMutex(ra_ledger_mutex);

	/* Finalize hash chain — must be done in order, under the mutex */
	memcpy(rec->prev_hash, ra_ledger_last_hash, SHA256_DIGEST_SIZE);
	ledger_compute_record_hash(rec);

	/* Update in-memory chain state immediately so the next enqueue sees it */
	ledger_hash_record(rec, ra_ledger_last_hash);
	ra_ledger_has_records = true;

	/* Enqueue for async disk write */
	LedgerWriteQueue* wq = &ra_ledger_wq;
	bool enqueued = false;

	SDL_LockMutex(wq->mutex);
	if (wq->count < RA_LEDGER_WRITE_QUEUE_SIZE) {
		wq->records[wq->tail] = *rec;
		wq->tail = (wq->tail + 1) % RA_LEDGER_WRITE_QUEUE_SIZE;
		wq->count++;
		SDL_CondSignal(wq->cond_nonempty);
		enqueued = true;
	} else {
		OFFLINE_LOG_ERROR("Ledger write queue full — record dropped!\n");
	}
	SDL_UnlockMutex(wq->mutex);

	SDL_UnlockMutex(ra_ledger_mutex);
	return enqueued;
}

/* Initialize a ledger record with common fields zeroed and set */
static void ledger_record_init(RA_LedgerRecord* rec, uint8_t type,
                               uint32_t game_id, uint32_t achievement_id,
                               uint8_t hardcore, const char* game_hash) {
	memset(rec, 0, sizeof(*rec));
	rec->type = type;
	rec->timestamp = (uint32_t)time(NULL);
	rec->game_id = game_id;
	rec->achievement_id = achievement_id;
	rec->hardcore = hardcore;
	if (game_hash) {
		strncpy(rec->game_hash, game_hash, sizeof(rec->game_hash) - 1);
	}
}

void RA_Offline_ledgerWriteSessionStart(uint32_t game_id, const char* game_hash,
                                        uint8_t hardcore) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	ledger_record_init(&rec, RA_LEDGER_SESSION_START, game_id, 0, hardcore, game_hash);

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SESSION_START game=%u hash=%s\n", game_id,
		                  game_hash ? game_hash : "(null)");
	}
}

void RA_Offline_ledgerWriteUnlock(uint32_t game_id, uint32_t achievement_id,
                                  const char* game_hash, uint8_t hardcore) {
	if (!ra_offline_initialized) return;

	/* Hardcore unlocks are never written to the offline ledger — they cannot
	 * be synced retroactively and would persist forever after compaction. */
	if (hardcore) {
		OFFLINE_LOG_DEBUG("Ledger: skipping hardcore UNLOCK achievement=%u game=%u\n",
		                  achievement_id, game_id);
		return;
	}

	RA_LedgerRecord rec;
	ledger_record_init(&rec, RA_LEDGER_ACHIEVEMENT_UNLOCK, game_id, achievement_id, 0, game_hash);

	if (ledger_append(&rec)) {
		OFFLINE_LOG_INFO("Ledger: UNLOCK achievement=%u game=%u timestamp=%u (time_now=%lld)\n",
		                 achievement_id, game_id, rec.timestamp, (long long)time(NULL));
	}
}

void RA_Offline_ledgerWriteSessionEnd(uint32_t game_id, const char* game_hash) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	ledger_record_init(&rec, RA_LEDGER_SESSION_END, game_id, 0, 0, game_hash);

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SESSION_END game=%u\n", game_id);
	}
}

void RA_Offline_ledgerWriteSyncAck(uint32_t achievement_id, uint32_t game_id) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	ledger_record_init(&rec, RA_LEDGER_SYNC_ACK, game_id, achievement_id, 0, NULL);

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SYNC_ACK achievement=%u game=%u\n",
		                  achievement_id, game_id);
	}
}

/*
 * Read the ledger and return only the pending (un-cancelled) softcore UNLOCK records.
 * Implements order-aware SYNC_ACK matching: each SYNC_ACK cancels the earliest
 * preceding unmatched UNLOCK with the same achievement ID.
 *
 * Caller MUST hold ra_ledger_mutex.
 *
 * On success: returns true, *out_records and *out_count are set.
 *             Caller must free(*out_records) when done.
 *             *out_count == 0 is a valid success (no pending records).
 * On failure: returns false (allocation error).
 */
static bool ledger_read_pending_records(RA_LedgerRecord** out_records,
                                        uint32_t* out_count) {
	*out_records = NULL;
	*out_count = 0;

	FILE* f = fopen(ra_ledger_path, "rb");
	if (!f) return true; /* No ledger = no pending records (not an error) */

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || (file_size % sizeof(RA_LedgerRecord)) != 0) {
		fclose(f);
		return true;
	}

	uint32_t total_records = (uint32_t)(file_size / (long)sizeof(RA_LedgerRecord));
	RA_LedgerRecord* records = (RA_LedgerRecord*)malloc(total_records * sizeof(RA_LedgerRecord));
	if (!records) {
		fclose(f);
		return false;
	}

	uint32_t read_count = (uint32_t)fread(records, sizeof(RA_LedgerRecord), total_records, f);
	fclose(f);

	if (read_count < total_records) {
		total_records = read_count;
	}

	if (total_records == 0) {
		free(records);
		return true;
	}

	/* Order-aware SYNC_ACK matching using a separate boolean array */
	bool* cancelled = (bool*)calloc(total_records, sizeof(bool));
	if (!cancelled) {
		free(records);
		return false;
	}

	for (uint32_t i = 0; i < total_records; i++) {
		if (records[i].type == RA_LEDGER_SYNC_ACK) {
			for (uint32_t j = 0; j < i; j++) {
				if (records[j].type == RA_LEDGER_ACHIEVEMENT_UNLOCK &&
				    records[j].achievement_id == records[i].achievement_id &&
				    !cancelled[j]) {
					cancelled[j] = true;
					break; /* only cancel ONE unlock per SYNC_ACK */
				}
			}
		}
	}

	/* Collect pending softcore UNLOCK records */
	RA_LedgerRecord* pending = (RA_LedgerRecord*)malloc(total_records * sizeof(RA_LedgerRecord));
	if (!pending) {
		free(cancelled);
		free(records);
		return false;
	}

	uint32_t pending_count = 0;
	for (uint32_t i = 0; i < total_records; i++) {
		if (records[i].type == RA_LEDGER_ACHIEVEMENT_UNLOCK &&
		    records[i].hardcore == 0 &&
		    !cancelled[i]) {
			pending[pending_count++] = records[i];
		}
	}

	free(cancelled);
	free(records);

	if (pending_count == 0) {
		free(pending);
		return true;
	}

	*out_records = pending;
	*out_count = pending_count;
	return true;
}

void RA_Offline_ledgerCompact(void) {
	if (!ra_offline_initialized) return;

	SDL_LockMutex(ra_ledger_mutex);

	/* Read pending records using the shared matching logic */
	RA_LedgerRecord* kept = NULL;
	uint32_t kept_count = 0;
	if (!ledger_read_pending_records(&kept, &kept_count)) {
		OFFLINE_LOG_ERROR("Compact: failed to read pending records\n");
		SDL_UnlockMutex(ra_ledger_mutex);
		return;
	}

	if (kept_count == 0) {
		/* Everything was acked (or no ledger) — delete the ledger */
		free(kept);
		if (unlink(ra_ledger_path) == 0) {
			OFFLINE_LOG_INFO("Compact: ledger fully synced, deleted\n");
		}
		/* If unlink fails, the file may not exist — that's fine */
		memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);
		ra_ledger_has_records = false;
		SDL_UnlockMutex(ra_ledger_mutex);
		return;
	}

	/* Rewrite the ledger with only the kept records, rebuilding the hash chain */
	OFFLINE_LOG_INFO("Compact: keeping %u pending records\n", kept_count);

	/* Write to a temp file, then rename for atomicity */
	char tmp_path[512];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ra_ledger_path);

	FILE* out = fopen(tmp_path, "wb");
	if (!out) {
		OFFLINE_LOG_ERROR("Compact: failed to create temp file: %s\n", strerror(errno));
		free(kept);
		SDL_UnlockMutex(ra_ledger_mutex);
		return;
	}

	/* Rebuild hash chain from scratch */
	uint8_t prev_hash[SHA256_DIGEST_SIZE];
	memset(prev_hash, 0, SHA256_DIGEST_SIZE);

	for (uint32_t i = 0; i < kept_count; i++) {
		/* Update chain links */
		memcpy(kept[i].prev_hash, prev_hash, SHA256_DIGEST_SIZE);
		ledger_compute_record_hash(&kept[i]);

		if (fwrite(&kept[i], sizeof(RA_LedgerRecord), 1, out) != 1) {
			OFFLINE_LOG_ERROR("Compact: write failed at record %u\n", i);
			fclose(out);
			unlink(tmp_path);
			free(kept);
			SDL_UnlockMutex(ra_ledger_mutex);
			return;
		}

		/* Advance chain */
		ledger_hash_record(&kept[i], prev_hash);
	}

	fflush(out);
	fsync(fileno(out));
	fclose(out);

	/* Atomic replace */
	if (rename(tmp_path, ra_ledger_path) != 0) {
		OFFLINE_LOG_ERROR("Compact: rename failed: %s\n", strerror(errno));
		unlink(tmp_path);
		free(kept);
		SDL_UnlockMutex(ra_ledger_mutex);
		return;
	}

	/* Update in-memory chain state */
	memcpy(ra_ledger_last_hash, prev_hash, SHA256_DIGEST_SIZE);
	ra_ledger_has_records = true;

	OFFLINE_LOG_INFO("Compact: rewrote ledger with %u records\n", kept_count);
	free(kept);
	SDL_UnlockMutex(ra_ledger_mutex);
}

bool RA_Offline_ledgerGetPendingUnlocks(RA_PendingUnlock** out_unlocks,
                                        uint32_t* out_count) {
	if (!ra_offline_initialized || !out_unlocks || !out_count) return false;

	SDL_LockMutex(ra_ledger_mutex);

	*out_unlocks = NULL;
	*out_count = 0;

	/* Use the shared matching logic to get pending records */
	RA_LedgerRecord* records = NULL;
	uint32_t record_count = 0;
	if (!ledger_read_pending_records(&records, &record_count)) {
		SDL_UnlockMutex(ra_ledger_mutex);
		return false;
	}

	if (record_count == 0) {
		SDL_UnlockMutex(ra_ledger_mutex);
		return true;
	}

	/* Convert RA_LedgerRecord to RA_PendingUnlock */
	RA_PendingUnlock* unlocks = (RA_PendingUnlock*)malloc(record_count * sizeof(RA_PendingUnlock));
	if (!unlocks) {
		free(records);
		SDL_UnlockMutex(ra_ledger_mutex);
		return false;
	}

	for (uint32_t i = 0; i < record_count; i++) {
		unlocks[i].achievement_id = records[i].achievement_id;
		unlocks[i].game_id = records[i].game_id;
		unlocks[i].timestamp = records[i].timestamp;
		unlocks[i].hardcore = 0;
		memcpy(unlocks[i].game_hash, records[i].game_hash, sizeof(records[i].game_hash));
	}
	free(records);

	*out_unlocks = unlocks;
	*out_count = record_count;
	OFFLINE_LOG_DEBUG("Ledger: %u pending softcore unlocks\n", record_count);
	SDL_UnlockMutex(ra_ledger_mutex);
	return true;
}

/*****************************************************************************
 * Offline mode state
 *****************************************************************************/

bool RA_Offline_isOffline(void) {
	return ra_offline_mode;
}

void RA_Offline_setOffline(bool offline) {
	if (ra_offline_mode != offline) {
		ra_offline_mode = offline;
		OFFLINE_LOG_INFO("Offline mode: %s\n", offline ? "ON" : "OFF");
	}
}

/*****************************************************************************
 * Sync engine state
 *****************************************************************************/

bool RA_Offline_isSyncing(void) {
	return ra_sync_in_progress;
}

void RA_Offline_setSyncing(bool syncing) {
	ra_sync_in_progress = syncing;
}

/*****************************************************************************
 * Initialization / shutdown
 *****************************************************************************/

void RA_Offline_init(const char* data_dir) {
	if (ra_offline_initialized) return;

	/* Store base paths */
	snprintf(ra_data_dir, sizeof(ra_data_dir), "%s%s", data_dir, RA_OFFLINE_DIR);
	snprintf(ra_cache_dir, sizeof(ra_cache_dir), "%s%s", data_dir, RA_CACHE_DIR);
	snprintf(ra_ledger_path, sizeof(ra_ledger_path), "%s%s", data_dir, RA_LEDGER_FILE);

	/* Create directories */
	if (mkdirs(ra_cache_dir) != 0) {
		OFFLINE_LOG_ERROR("Failed to create cache directory: %s (%s)\n",
		                  ra_cache_dir, strerror(errno));
		/* Continue anyway - caching will fail gracefully */
	}

	/* Validate and load ledger */
	ledger_validate_and_load();

	/* Create ledger mutex for thread-safe access */
	ra_ledger_mutex = SDL_CreateMutex();
	if (!ra_ledger_mutex) {
		OFFLINE_LOG_ERROR("Failed to create ledger mutex: %s\n", SDL_GetError());
	}

	/* Start the background ledger writer thread */
	ledger_writer_start();

	ra_offline_initialized = true;
	OFFLINE_LOG_INFO("Initialized (cache: %s, ledger: %s)\n", ra_cache_dir, ra_ledger_path);
}

void RA_Offline_shutdown(void) {
	if (!ra_offline_initialized) return;

	/* Stop the background writer first — flushes any queued records to disk */
	ledger_writer_stop();

	ra_offline_initialized = false;
	ra_offline_mode = false;
	ra_sync_in_progress = false;
	ra_ledger_has_records = false;
	ra_pending_count = 0;
	memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);

	if (ra_ledger_mutex) {
		SDL_DestroyMutex(ra_ledger_mutex);
		ra_ledger_mutex = NULL;
	}

	OFFLINE_LOG_INFO("Shut down\n");
}

/*****************************************************************************
 * Pending offline unlock cache (for UI queries)
 *****************************************************************************/

void RA_Offline_refreshPendingCache(void) {
	/* Read pending unlocks (ledgerGetPendingUnlocks locks internally) */
	RA_PendingUnlock* unlocks = NULL;
	uint32_t count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&unlocks, &count) || count == 0) {
		SDL_LockMutex(ra_ledger_mutex);
		ra_pending_count = 0;
		SDL_UnlockMutex(ra_ledger_mutex);
		free(unlocks);
		return;
	}

	SDL_LockMutex(ra_ledger_mutex);
	uint32_t to_cache = count < RA_MAX_PENDING_CACHE ? count : RA_MAX_PENDING_CACHE;
	if (count > RA_MAX_PENDING_CACHE) {
		OFFLINE_LOG_WARN("Pending cache truncated: %u entries, max %u\n",
		                 count, RA_MAX_PENDING_CACHE);
	}
	for (uint32_t i = 0; i < to_cache; i++) {
		ra_pending_ids[i] = unlocks[i].achievement_id;
	}
	ra_pending_count = to_cache;
	SDL_UnlockMutex(ra_ledger_mutex);
	free(unlocks);

	OFFLINE_LOG_DEBUG("Pending cache refreshed: %u entries\n", to_cache);
}

bool RA_Offline_isUnlockPending(uint32_t achievement_id) {
	SDL_LockMutex(ra_ledger_mutex);
	bool found = false;
	for (uint32_t i = 0; i < ra_pending_count; i++) {
		if (ra_pending_ids[i] == achievement_id) {
			found = true;
			break;
		}
	}
	SDL_UnlockMutex(ra_ledger_mutex);
	return found;
}

void RA_Offline_addPendingCacheEntry(uint32_t achievement_id) {
	SDL_LockMutex(ra_ledger_mutex);
	/* Check for duplicates */
	for (uint32_t i = 0; i < ra_pending_count; i++) {
		if (ra_pending_ids[i] == achievement_id) {
			SDL_UnlockMutex(ra_ledger_mutex);
			return;
		}
	}
	if (ra_pending_count < RA_MAX_PENDING_CACHE) {
		ra_pending_ids[ra_pending_count++] = achievement_id;
	} else {
		OFFLINE_LOG_WARN("Pending cache full (%u entries), achievement %u not cached for UI\n",
		                 RA_MAX_PENDING_CACHE, achievement_id);
	}
	SDL_UnlockMutex(ra_ledger_mutex);
}

void RA_Offline_removePendingCacheEntry(uint32_t achievement_id) {
	SDL_LockMutex(ra_ledger_mutex);
	for (uint32_t i = 0; i < ra_pending_count; i++) {
		if (ra_pending_ids[i] == achievement_id) {
			/* Shift remaining elements down */
			for (uint32_t j = i; j < ra_pending_count - 1; j++) {
				ra_pending_ids[j] = ra_pending_ids[j + 1];
			}
			ra_pending_count--;
			break;
		}
	}
	SDL_UnlockMutex(ra_ledger_mutex);
}

void RA_Offline_clearPendingCache(void) {
	SDL_LockMutex(ra_ledger_mutex);
	ra_pending_count = 0;
	SDL_UnlockMutex(ra_ledger_mutex);
}

void RA_Offline_patchStartsessionCacheWithUnlock(const char* game_hash,
                                                  uint32_t achievement_id,
                                                  uint32_t timestamp) {
	if (!ra_offline_initialized || !game_hash || game_hash[0] == '\0') return;

	/* Build cache file path: <cache_dir>/startsession_<hash>.bin */
	char path[512];
	snprintf(path, sizeof(path), "%s/startsession_%s.bin", ra_cache_dir, game_hash);

	/* Read existing cache file: [uint32_t len][body][SHA-256] */
	FILE* f = fopen(path, "rb");
	if (!f) {
		/* No cached startsession for this game — nothing to patch */
		return;
	}

	uint32_t len32;
	if (fread(&len32, sizeof(len32), 1, f) != 1 || len32 > 8 * 1024 * 1024) {
		fclose(f);
		return;
	}

	char* body = (char*)malloc(len32 + 1);
	if (!body) {
		fclose(f);
		return;
	}

	if (fread(body, 1, len32, f) != len32) {
		free(body);
		fclose(f);
		return;
	}
	body[len32] = '\0';

	uint8_t stored_digest[SHA256_DIGEST_SIZE];
	if (fread(stored_digest, SHA256_DIGEST_SIZE, 1, f) != 1) {
		free(body);
		fclose(f);
		return;
	}
	fclose(f);

	/* Verify SHA-256 integrity */
	uint8_t computed_digest[SHA256_DIGEST_SIZE];
	sha256_hash(body, len32, computed_digest);
	if (memcmp(stored_digest, computed_digest, SHA256_DIGEST_SIZE) != 0) {
		OFFLINE_LOG_WARN("patchCache: SHA-256 mismatch for %s, skipping\n", path);
		free(body);
		return;
	}

	/* Find the "Unlocks" array */
	const char* unlocks_key = "\"Unlocks\"";
	char* unlocks_pos = strstr(body, unlocks_key);
	if (!unlocks_pos) {
		free(body);
		return;
	}

	char* arr_start = strchr(unlocks_pos + strlen(unlocks_key), '[');
	if (!arr_start) {
		free(body);
		return;
	}

	char* arr_end = strchr(arr_start, ']');
	if (!arr_end) {
		free(body);
		return;
	}

	/* Check if this achievement ID is already in the Unlocks array */
	char id_pattern[32];
	snprintf(id_pattern, sizeof(id_pattern), "\"ID\":%u", achievement_id);
	bool already_in_softcore = false;
	for (char* p = arr_start; p < arr_end; p++) {
		if (strncmp(p, id_pattern, strlen(id_pattern)) == 0) {
			already_in_softcore = true;
			break;
		}
	}

	/* Also check HardcoreUnlocks array */
	bool already_in_hardcore = false;
	const char* hc_key = "\"HardcoreUnlocks\"";
	char* hc_pos = strstr(body, hc_key);
	if (hc_pos) {
		char* hc_arr_start = strchr(hc_pos + strlen(hc_key), '[');
		if (hc_arr_start) {
			char* hc_arr_end = strchr(hc_arr_start, ']');
			if (hc_arr_end) {
				for (char* p = hc_arr_start; p < hc_arr_end; p++) {
					if (strncmp(p, id_pattern, strlen(id_pattern)) == 0) {
						already_in_hardcore = true;
						break;
					}
				}
			}
		}
	}

	if (already_in_softcore && already_in_hardcore) {
		/* Already present in both arrays — no-op */
		free(body);
		return;
	}

	/*
	 * Use inject_unlocks_into_array for each array that needs patching.
	 * We create a temporary single-element pending array to reuse the helper.
	 */
	RA_PendingUnlock single;
	single.achievement_id = achievement_id;
	single.timestamp = timestamp;
	single.game_id = 0;
	single.hardcore = 0;
	/* game_hash filter: use empty string so the filter is bypassed
	 * (inject_unlocks_into_array skips entries where game_hash doesn't match,
	 *  but if we pass NULL as game_hash it accepts all entries). */
	single.game_hash[0] = '\0';

	char* current_body = body;
	size_t current_len = (size_t)len32;
	bool body_replaced = false;

	/* Inject into "Unlocks" if not already present */
	if (!already_in_softcore) {
		char* new_body = NULL;
		size_t new_len = 0;
		uint32_t injected = inject_unlocks_into_array(
			current_body, current_len, "\"Unlocks\"",
			&single, 1, NULL,
			&new_body, &new_len);
		if (injected > 0 && new_body) {
			if (body_replaced) free(current_body);
			current_body = new_body;
			current_len = new_len;
			body_replaced = true;
		}
	}

	/* Inject into "HardcoreUnlocks" if not already present */
	if (!already_in_hardcore) {
		char* new_body = NULL;
		size_t new_len = 0;
		uint32_t injected = inject_unlocks_into_array(
			current_body, current_len, "\"HardcoreUnlocks\"",
			&single, 1, NULL,
			&new_body, &new_len);
		if (injected > 0 && new_body) {
			if (body_replaced) free(current_body);
			current_body = new_body;
			current_len = new_len;
			body_replaced = true;
		}
	}

	if (!body_replaced) {
		/* Nothing was injected (shouldn't happen given the checks above) */
		free(body);
		return;
	}

	/* Free the original body (if not already freed via body_replaced logic) */
	if (body_replaced && current_body != body) {
		free(body);
	}

	/* Rewrite cache file with updated body and new SHA-256 */
	uint8_t new_digest[SHA256_DIGEST_SIZE];
	sha256_hash(current_body, current_len, new_digest);

	f = fopen(path, "wb");
	if (!f) {
		OFFLINE_LOG_ERROR("patchCache: failed to open %s for rewrite: %s\n",
		                  path, strerror(errno));
		free(current_body);
		return;
	}

	uint32_t new_len32 = (uint32_t)current_len;
	size_t written = 0;
	written += fwrite(&new_len32, sizeof(new_len32), 1, f);
	written += fwrite(current_body, 1, current_len, f) > 0 ? 1 : 0;
	written += fwrite(new_digest, SHA256_DIGEST_SIZE, 1, f);

	if (written < 3) {
		OFFLINE_LOG_ERROR("patchCache: write failed for %s\n", path);
		fclose(f);
		unlink(path);
		free(current_body);
		return;
	}

	fflush(f);
	fsync(fileno(f));
	fclose(f);
	free(current_body);

	OFFLINE_LOG_INFO("patchCache: injected achievement %u into %s\n",
	                 achievement_id, path);
}


#include "ra_offline.h"
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
 * SHA-256 implementation (self-contained, no external dependencies)
 *
 * Based on FIPS 180-4. Public domain reference style.
 *****************************************************************************/

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
	uint32_t state[8];
	uint64_t count;
	uint8_t  buffer[SHA256_BLOCK_SIZE];
} SHA256_CTX;

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(SHA256_CTX* ctx, const uint8_t* data) {
	uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)data[i * 4] << 24) |
		       ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) |
		       ((uint32_t)data[i * 4 + 3]);
	}
	for (i = 16; i < 64; i++) {
		w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
	}

	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		t1 = h + EP1(e) + CH(e, f, g) + sha256_k[i] + w[i];
		t2 = EP0(a) + MAJ(a, b, c);
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX* ctx) {
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
	ctx->count = 0;
}

static void sha256_update(SHA256_CTX* ctx, const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	size_t fill = (size_t)(ctx->count & 0x3f);
	ctx->count += len;

	if (fill) {
		size_t avail = SHA256_BLOCK_SIZE - fill;
		if (len < avail) {
			memcpy(ctx->buffer + fill, p, len);
			return;
		}
		memcpy(ctx->buffer + fill, p, avail);
		sha256_transform(ctx, ctx->buffer);
		p += avail;
		len -= avail;
	}
	while (len >= SHA256_BLOCK_SIZE) {
		sha256_transform(ctx, p);
		p += SHA256_BLOCK_SIZE;
		len -= SHA256_BLOCK_SIZE;
	}
	if (len) {
		memcpy(ctx->buffer, p, len);
	}
}

static void sha256_final(SHA256_CTX* ctx, uint8_t digest[SHA256_DIGEST_SIZE]) {
	uint64_t bits = ctx->count * 8;
	size_t fill = (size_t)(ctx->count & 0x3f);
	int i;

	/* Padding */
	ctx->buffer[fill++] = 0x80;
	if (fill > 56) {
		memset(ctx->buffer + fill, 0, SHA256_BLOCK_SIZE - fill);
		sha256_transform(ctx, ctx->buffer);
		fill = 0;
	}
	memset(ctx->buffer + fill, 0, 56 - fill);

	/* Length in bits, big-endian */
	for (i = 0; i < 8; i++) {
		ctx->buffer[56 + i] = (uint8_t)(bits >> (56 - i * 8));
	}
	sha256_transform(ctx, ctx->buffer);

	/* Output */
	for (i = 0; i < 8; i++) {
		digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

/* Convenience: hash a buffer in one shot */
static void sha256_hash(const void* data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]) {
	SHA256_CTX ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
}

/*****************************************************************************
 * Static state
 *****************************************************************************/

static bool ra_offline_initialized = false;
static bool ra_offline_mode = false;
static bool ra_sync_in_progress = false;

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
static const char* get_request_type(const char* url, const char* post_data) {
	static char type_buf[32];

	/* Try URL first (GET requests have ?r=... in URL) */
	if (url && extract_param(url, "r", type_buf, sizeof(type_buf))) {
		return type_buf;
	}
	/* Try POST data */
	if (post_data && extract_param(post_data, "r", type_buf, sizeof(type_buf))) {
		return type_buf;
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

	const char* req_type = get_request_type(url, post_data);
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
 * we inject them into the "Unlocks" array so rcheevos shows them as unlocked.
 *
 * @param body        The cached JSON body (will be freed and replaced)
 * @param body_len    Length of body
 * @param game_hash   Game hash to filter ledger entries by
 * @param out_body    Output: patched body (caller must free)
 * @param out_len     Output: patched body length
 * @return true if patching succeeded (or no patching needed), false on error
 */
static bool patch_startsession_with_ledger(char* body, size_t body_len,
                                           const char* game_hash,
                                           char** out_body, size_t* out_len) {
	/* Get pending unlocks from ledger */
	RA_PendingUnlock* pending = NULL;
	uint32_t pending_count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&pending, &pending_count)) {
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	if (pending_count == 0 || !pending) {
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	/* Filter to only unlocks for this game hash */
	uint32_t game_count = 0;
	for (uint32_t i = 0; i < pending_count; i++) {
		if (game_hash && strcmp(pending[i].game_hash, game_hash) == 0) {
			game_count++;
		}
	}

	if (game_count == 0) {
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	/*
	 * Parse existing Unlocks array to avoid duplicates.
	 * Format: "Unlocks":[{"ID":123,"When":456},...] or "Unlocks":[]
	 */
	const char* unlocks_key = "\"Unlocks\"";
	char* unlocks_pos = strstr(body, unlocks_key);
	if (!unlocks_pos) {
		/* No Unlocks field — very unusual, just return as-is */
		OFFLINE_LOG_WARN("startsession response has no Unlocks field\n");
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	/* Find the '[' after "Unlocks" */
	char* arr_start = strchr(unlocks_pos + strlen(unlocks_key), '[');
	if (!arr_start) {
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	/* Find the matching ']' */
	char* arr_end = strchr(arr_start, ']');
	if (!arr_end) {
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	/* Check which pending achievement IDs are already in the Unlocks array */
	/* Simple approach: search for "ID":NNNN in the existing array text */
	bool* need_inject = (bool*)calloc(pending_count, sizeof(bool));
	if (!need_inject) {
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	uint32_t inject_count = 0;
	size_t arr_text_len = (size_t)(arr_end - arr_start);
	for (uint32_t i = 0; i < pending_count; i++) {
		if (!game_hash || strcmp(pending[i].game_hash, game_hash) != 0) {
			continue;
		}
		/* Check if this ID already exists in the array */
		char id_pattern[32];
		snprintf(id_pattern, sizeof(id_pattern), "\"ID\":%u", pending[i].achievement_id);
		bool found = false;
		/* Search only within the existing array bounds */
		for (char* p = arr_start; p < arr_end; p++) {
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
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	OFFLINE_LOG_DEBUG("Patching startsession: injecting %u ledger unlocks into Unlocks array\n",
	                 inject_count);

	/*
	 * Build the injection string.
	 * Each entry: {"ID":NNNN,"When":TTTTTTTTTT}
	 * Max per entry: ~40 chars. Allocate generously.
	 */
	size_t inject_buf_size = inject_count * 48 + 1;
	char* inject_buf = (char*)malloc(inject_buf_size);
	if (!inject_buf) {
		free(need_inject);
		free(pending);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	size_t inject_len = 0;
	bool existing_entries = (arr_end - arr_start > 1); /* true if array is non-empty */
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
	free(pending);

	/* Build the new body: prefix + inject + suffix */
	/* Insert just before the ']' of the Unlocks array */
	size_t prefix_len = (size_t)(arr_end - body);
	size_t suffix_len = body_len - prefix_len; /* includes ']' and everything after */
	size_t new_len = prefix_len + inject_len + suffix_len;
	char* new_body = (char*)malloc(new_len + 1);
	if (!new_body) {
		free(inject_buf);
		*out_body = body;
		*out_len = body_len;
		return true;
	}

	memcpy(new_body, body, prefix_len);
	memcpy(new_body + prefix_len, inject_buf, inject_len);
	memcpy(new_body + prefix_len + inject_len, arr_end, suffix_len);
	new_body[new_len] = '\0';
	free(inject_buf);

	OFFLINE_LOG_DEBUG("Patched startsession: %zu -> %zu bytes\n", body_len, new_len);

	/* Replace the original body */
	free(body);
	*out_body = new_body;
	*out_len = new_len;
	return true;
}

bool RA_Offline_patchStartsessionResponse(char* body, size_t body_len,
                                          const char* game_hash,
                                          char** out_body, size_t* out_len) {
	if (!ra_offline_initialized || !body || body_len == 0) return false;

	/* Check if there are any pending unlocks for this game before doing work */
	RA_PendingUnlock* pending = NULL;
	uint32_t pending_count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&pending, &pending_count) ||
	    pending_count == 0) {
		free(pending);
		return false;
	}

	/* Check if any pending unlocks match this game hash */
	bool has_match = false;
	for (uint32_t i = 0; i < pending_count; i++) {
		if (game_hash && strcmp(pending[i].game_hash, game_hash) == 0) {
			has_match = true;
			break;
		}
	}
	free(pending);

	if (!has_match) return false;

	/* Delegate to the internal patching function */
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

	const char* req_type = get_request_type(url, post_data);
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

	/* For startsession responses served offline, patch in any pending ledger unlocks
	 * so rcheevos shows offline-earned achievements as unlocked on restart */
	if (ra_offline_mode && strcmp(req_type, "startsession") == 0) {
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
 * Append a record to the ledger file.
 */
static bool ledger_append(RA_LedgerRecord* rec) {
	/* Set prev_hash from chain state */
	memcpy(rec->prev_hash, ra_ledger_last_hash, SHA256_DIGEST_SIZE);

	/* Compute record_hash */
	ledger_compute_record_hash(rec);

	/* Open for append */
	FILE* f = fopen(ra_ledger_path, "ab");
	if (!f) {
		OFFLINE_LOG_ERROR("Failed to open ledger for append: %s\n", strerror(errno));
		return false;
	}

	if (fwrite(rec, sizeof(RA_LedgerRecord), 1, f) != 1) {
		OFFLINE_LOG_ERROR("Failed to write ledger record: %s\n", strerror(errno));
		fclose(f);
		return false;
	}

	fflush(f);
	fsync(fileno(f));
	fclose(f);

	/* Update chain state */
	ledger_hash_record(rec, ra_ledger_last_hash);
	ra_ledger_has_records = true;

	return true;
}

void RA_Offline_ledgerWriteSessionStart(uint32_t game_id, const char* game_hash,
                                        uint8_t hardcore) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	memset(&rec, 0, sizeof(rec));
	rec.type = RA_LEDGER_SESSION_START;
	rec.timestamp = (uint32_t)time(NULL);
	rec.game_id = game_id;
	rec.achievement_id = 0;
	rec.hardcore = hardcore;
	if (game_hash) {
		strncpy(rec.game_hash, game_hash, sizeof(rec.game_hash) - 1);
	}

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SESSION_START game=%u hash=%s\n", game_id,
		                  game_hash ? game_hash : "(null)");
	}
}

void RA_Offline_ledgerWriteUnlock(uint32_t game_id, uint32_t achievement_id,
                                  const char* game_hash, uint8_t hardcore) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	memset(&rec, 0, sizeof(rec));
	rec.type = RA_LEDGER_ACHIEVEMENT_UNLOCK;
	rec.timestamp = (uint32_t)time(NULL);
	rec.game_id = game_id;
	rec.achievement_id = achievement_id;
	rec.hardcore = hardcore;
	if (game_hash) {
		strncpy(rec.game_hash, game_hash, sizeof(rec.game_hash) - 1);
	}

	if (ledger_append(&rec)) {
		OFFLINE_LOG_INFO("Ledger: UNLOCK achievement=%u game=%u hardcore=%d\n",
		                 achievement_id, game_id, hardcore);
	}
}

void RA_Offline_ledgerWriteSessionEnd(uint32_t game_id, const char* game_hash) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	memset(&rec, 0, sizeof(rec));
	rec.type = RA_LEDGER_SESSION_END;
	rec.timestamp = (uint32_t)time(NULL);
	rec.game_id = game_id;
	rec.achievement_id = 0;
	rec.hardcore = 0;
	if (game_hash) {
		strncpy(rec.game_hash, game_hash, sizeof(rec.game_hash) - 1);
	}

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SESSION_END game=%u\n", game_id);
	}
}

void RA_Offline_ledgerWriteSyncAck(uint32_t achievement_id, uint32_t game_id) {
	if (!ra_offline_initialized) return;

	RA_LedgerRecord rec;
	memset(&rec, 0, sizeof(rec));
	rec.type = RA_LEDGER_SYNC_ACK;
	rec.timestamp = (uint32_t)time(NULL);
	rec.game_id = game_id;
	rec.achievement_id = achievement_id;
	rec.hardcore = 0;

	if (ledger_append(&rec)) {
		OFFLINE_LOG_DEBUG("Ledger: SYNC_ACK achievement=%u game=%u\n",
		                  achievement_id, game_id);
	}
}

void RA_Offline_ledgerCompact(void) {
	if (!ra_offline_initialized) return;

	FILE* f = fopen(ra_ledger_path, "rb");
	if (!f) {
		OFFLINE_LOG_DEBUG("Compact: no ledger file to compact\n");
		return;
	}

	/* Read all records (ignoring chain integrity — we just want the data) */
	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || (file_size % sizeof(RA_LedgerRecord)) != 0) {
		fclose(f);
		OFFLINE_LOG_WARN("Compact: ledger file has invalid size %ld, deleting\n", file_size);
		unlink(ra_ledger_path);
		memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);
		ra_ledger_has_records = false;
		return;
	}

	uint32_t total_records = (uint32_t)(file_size / (long)sizeof(RA_LedgerRecord));
	RA_LedgerRecord* records = (RA_LedgerRecord*)malloc(total_records * sizeof(RA_LedgerRecord));
	if (!records) {
		fclose(f);
		OFFLINE_LOG_ERROR("Compact: failed to allocate for %u records\n", total_records);
		return;
	}

	uint32_t read_count = (uint32_t)fread(records, sizeof(RA_LedgerRecord), total_records, f);
	fclose(f);

	if (read_count < total_records) {
		OFFLINE_LOG_WARN("Compact: read only %u of %u records\n", read_count, total_records);
		total_records = read_count;
	}

	/* Order-aware SYNC_ACK matching:
	 * For each SYNC_ACK, find and mark the EARLIEST preceding unmatched UNLOCK
	 * with the same achievement ID. This ensures that a re-unlock AFTER a
	 * SYNC_ACK remains pending. We use the prev_hash field as a scratch "marked"
	 * flag — it will be rebuilt when we rewrite the chain anyway.
	 *
	 * We repurpose prev_hash[0] as a "cancelled" flag:
	 *   0xFF = this UNLOCK has been cancelled by a later SYNC_ACK
	 */

	/* First, clear all cancel marks */
	for (uint32_t i = 0; i < total_records; i++) {
		if (records[i].type == RA_LEDGER_ACHIEVEMENT_UNLOCK) {
			records[i].prev_hash[0] = 0x00; /* not cancelled */
		}
	}

	/* For each SYNC_ACK, cancel the earliest un-cancelled UNLOCK before it */
	for (uint32_t i = 0; i < total_records; i++) {
		if (records[i].type == RA_LEDGER_SYNC_ACK) {
			for (uint32_t j = 0; j < i; j++) {
				if (records[j].type == RA_LEDGER_ACHIEVEMENT_UNLOCK &&
				    records[j].achievement_id == records[i].achievement_id &&
				    records[j].prev_hash[0] != 0xFF) {
					records[j].prev_hash[0] = 0xFF; /* mark cancelled */
					break; /* only cancel ONE unlock per SYNC_ACK */
				}
			}
		}
	}

	/* Keep only UNLOCK records that were NOT cancelled */
	RA_LedgerRecord* kept = NULL;
	uint32_t kept_count = 0;

	for (uint32_t i = 0; i < total_records; i++) {
		if (records[i].type == RA_LEDGER_ACHIEVEMENT_UNLOCK &&
		    records[i].prev_hash[0] != 0xFF) {
			/* Still pending — keep it */
			if (!kept) {
				kept = (RA_LedgerRecord*)malloc(total_records * sizeof(RA_LedgerRecord));
				if (!kept) {
					free(records);
					OFFLINE_LOG_ERROR("Compact: allocation failure for kept records\n");
					return;
				}
			}
			kept[kept_count++] = records[i];
		}
		/* All other record types (SESSION_START, SESSION_END, SYNC_ACK,
		 * and cancelled UNLOCKs) are dropped */
	}

	free(records);

	if (kept_count == 0) {
		/* Everything was acked — delete the ledger entirely */
		free(kept);
		if (unlink(ra_ledger_path) == 0) {
			OFFLINE_LOG_INFO("Compact: ledger fully synced, deleted (%u records removed)\n",
			                 total_records);
		} else {
			OFFLINE_LOG_WARN("Compact: failed to delete ledger: %s\n", strerror(errno));
		}
		memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);
		ra_ledger_has_records = false;
		return;
	}

	/* Rewrite the ledger with only the kept records, rebuilding the hash chain */
	OFFLINE_LOG_INFO("Compact: keeping %u pending records, dropping %u\n",
	                 kept_count, total_records - kept_count);

	/* Write to a temp file, then rename for atomicity */
	char tmp_path[512];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ra_ledger_path);

	FILE* out = fopen(tmp_path, "wb");
	if (!out) {
		OFFLINE_LOG_ERROR("Compact: failed to create temp file: %s\n", strerror(errno));
		free(kept);
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
		return;
	}

	/* Update in-memory chain state */
	memcpy(ra_ledger_last_hash, prev_hash, SHA256_DIGEST_SIZE);
	ra_ledger_has_records = true;

	OFFLINE_LOG_INFO("Compact: rewrote ledger with %u records\n", kept_count);
	free(kept);
}

bool RA_Offline_ledgerGetPendingUnlocks(RA_PendingUnlock** out_unlocks,
                                        uint32_t* out_count) {
	if (!ra_offline_initialized || !out_unlocks || !out_count) return false;

	*out_unlocks = NULL;
	*out_count = 0;

	FILE* f = fopen(ra_ledger_path, "rb");
	if (!f) return true; /* No ledger = no pending unlocks (not an error) */

	/*
	 * Strategy: Process records in sequential order. When we see an UNLOCK,
	 * add it to the pending list. When we see a SYNC_ACK, remove the EARLIEST
	 * pending UNLOCK with that achievement ID. This ensures a SYNC_ACK only
	 * cancels UNLOCKs that came BEFORE it, not ones added AFTER.
	 */

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || (file_size % sizeof(RA_LedgerRecord)) != 0) {
		fclose(f);
		return true;
	}

	uint32_t total_records = (uint32_t)(file_size / (long)sizeof(RA_LedgerRecord));

	RA_PendingUnlock* unlocks = NULL;
	uint32_t unlock_count = 0;
	uint32_t unlock_capacity = 0;

	RA_LedgerRecord rec;
	for (uint32_t i = 0; i < total_records; i++) {
		if (fread(&rec, sizeof(rec), 1, f) != 1) break;

		if (rec.type == RA_LEDGER_ACHIEVEMENT_UNLOCK && rec.hardcore == 0) {
			/* Softcore unlock — add to pending list */
			if (unlock_count >= unlock_capacity) {
				unlock_capacity = unlock_capacity == 0 ? 64 : unlock_capacity * 2;
				RA_PendingUnlock* tmp = (RA_PendingUnlock*)realloc(unlocks,
					unlock_capacity * sizeof(RA_PendingUnlock));
				if (!tmp) {
					free(unlocks);
					fclose(f);
					return false;
				}
				unlocks = tmp;
			}
			unlocks[unlock_count].achievement_id = rec.achievement_id;
			unlocks[unlock_count].game_id = rec.game_id;
			unlocks[unlock_count].timestamp = rec.timestamp;
			unlocks[unlock_count].hardcore = 0;
			memcpy(unlocks[unlock_count].game_hash, rec.game_hash, sizeof(rec.game_hash));
			unlock_count++;
		} else if (rec.type == RA_LEDGER_SYNC_ACK) {
			/* Cancel the EARLIEST pending unlock with this achievement ID.
			 * This is order-aware: only UNLOCKs already in the list (i.e.
			 * that appeared before this SYNC_ACK) can be cancelled. */
			for (uint32_t j = 0; j < unlock_count; j++) {
				if (unlocks[j].achievement_id == rec.achievement_id) {
					/* Remove by shifting remaining entries down */
					if (j + 1 < unlock_count) {
						memmove(&unlocks[j], &unlocks[j + 1],
						        (unlock_count - j - 1) * sizeof(RA_PendingUnlock));
					}
					unlock_count--;
					break; /* Only cancel ONE unlock per SYNC_ACK */
				}
			}
		}
	}
	fclose(f);

	if (unlock_count == 0) {
		free(unlocks);
		return true;
	}

	*out_unlocks = unlocks;
	*out_count = unlock_count;
	OFFLINE_LOG_DEBUG("Ledger: %u pending softcore unlocks\n", unlock_count);
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

	ra_offline_initialized = true;
	OFFLINE_LOG_INFO("Initialized (cache: %s, ledger: %s)\n", ra_cache_dir, ra_ledger_path);
}

void RA_Offline_shutdown(void) {
	if (!ra_offline_initialized) return;

	ra_offline_initialized = false;
	ra_offline_mode = false;
	ra_sync_in_progress = false;
	ra_ledger_has_records = false;
	ra_pending_count = 0;
	memset(ra_ledger_last_hash, 0, SHA256_DIGEST_SIZE);

	OFFLINE_LOG_INFO("Shut down\n");
}

/*****************************************************************************
 * Pending offline unlock cache (for UI queries)
 *****************************************************************************/

void RA_Offline_refreshPendingCache(void) {
	ra_pending_count = 0;

	RA_PendingUnlock* unlocks = NULL;
	uint32_t count = 0;
	if (!RA_Offline_ledgerGetPendingUnlocks(&unlocks, &count) || count == 0) {
		free(unlocks);
		return;
	}

	uint32_t to_cache = count < RA_MAX_PENDING_CACHE ? count : RA_MAX_PENDING_CACHE;
	for (uint32_t i = 0; i < to_cache; i++) {
		ra_pending_ids[i] = unlocks[i].achievement_id;
	}
	ra_pending_count = to_cache;
	free(unlocks);

	OFFLINE_LOG_DEBUG("Pending cache refreshed: %u entries\n", ra_pending_count);
}

bool RA_Offline_isUnlockPending(uint32_t achievement_id) {
	for (uint32_t i = 0; i < ra_pending_count; i++) {
		if (ra_pending_ids[i] == achievement_id) return true;
	}
	return false;
}

void RA_Offline_addPendingCacheEntry(uint32_t achievement_id) {
	/* Check for duplicates */
	for (uint32_t i = 0; i < ra_pending_count; i++) {
		if (ra_pending_ids[i] == achievement_id) return;
	}
	if (ra_pending_count < RA_MAX_PENDING_CACHE) {
		ra_pending_ids[ra_pending_count++] = achievement_id;
	}
}

void RA_Offline_clearPendingCache(void) {
	ra_pending_count = 0;
}

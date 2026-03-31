/*
 * Standalone RetroAchievements offline sync engine.
 *
 * Submits pending offline achievement unlocks to the RA server
 * without requiring the rcheevos library. Uses MD5 for the award
 * request signature and the existing HTTP/ledger infrastructure.
 *
 * Shared by both settings (batch sync all games) and minarch
 * (per-game sync on game load).
 */

#include "ra_sync.h"
#include "ra_offline.h"
#include "http.h"
#include "md5.h"
#include "config.h"
#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Logging macros using NextUI's LOG_* infrastructure */
#define SYNC_LOG_INFO(fmt, ...)  LOG_info("[RA_SYNC] " fmt, ##__VA_ARGS__)
#define SYNC_LOG_WARN(fmt, ...)  LOG_warn("[RA_SYNC] " fmt, ##__VA_ARGS__)
#define SYNC_LOG_ERROR(fmt, ...) LOG_error("[RA_SYNC] " fmt, ##__VA_ARGS__)

#define RA_API_URL "https://retroachievements.org/dorequest.php"

/* Default config used when NULL is passed */
static const RA_SyncConfig sync_default_config = RA_SYNC_CONFIG_INTERACTIVE;

/*****************************************************************************
 * Internal helpers
 *****************************************************************************/

static bool ra_sync_initialized = false;

static void sync_ensure_init(void) {
	if (!ra_sync_initialized) {
		RA_Offline_init(SHARED_USERDATA_PATH);
		ra_sync_initialized = true;
	}
}

/**
 * Generate a random delay in [min_ms, max_ms].
 */
static uint32_t sync_random_delay(uint32_t min_ms, uint32_t max_ms) {
	if (min_ms >= max_ms) return min_ms;
	return min_ms + (rand() % (max_ms - min_ms + 1));
}

/**
 * Sleep for the given number of milliseconds, checking cancel flag
 * every 100ms. Returns true if cancelled.
 */
static bool sync_interruptible_sleep(uint32_t ms, volatile bool* cancel) {
	uint32_t elapsed = 0;
	while (elapsed < ms) {
		if (cancel && *cancel) return true;
		uint32_t chunk = (ms - elapsed) > 100 ? 100 : (ms - elapsed);
		usleep(chunk * 1000);
		elapsed += chunk;
	}
	return (cancel && *cancel);
}

/**
 * Compute the MD5 signature for an award achievement request.
 *
 * The signature is: MD5(achievement_id_str + username + hardcore_str)
 * If seconds_since > 0, appends: + achievement_id_str + seconds_since_str
 *
 * Result is a 32-char lowercase hex string + null terminator.
 */
static void sync_compute_signature(uint32_t achievement_id,
                                   const char* username,
                                   uint8_t hardcore,
                                   uint32_t seconds_since,
                                   char sig_out[33]) {
	char id_str[16];
	char hc_str[2];
	char sec_str[16];

	snprintf(id_str, sizeof(id_str), "%u", achievement_id);
	snprintf(hc_str, sizeof(hc_str), "%u", hardcore ? 1 : 0);

	NUI_MD5_CTX ctx;
	nui_md5_init(&ctx);
	nui_md5_update(&ctx, id_str, strlen(id_str));
	nui_md5_update(&ctx, username, strlen(username));
	nui_md5_update(&ctx, hc_str, strlen(hc_str));

	if (seconds_since > 0) {
		snprintf(sec_str, sizeof(sec_str), "%u", seconds_since);
		nui_md5_update(&ctx, id_str, strlen(id_str));
		nui_md5_update(&ctx, sec_str, strlen(sec_str));
	}

	uint8_t digest[NUI_MD5_DIGEST_SIZE];
	nui_md5_final(&ctx, digest);

	for (int i = 0; i < NUI_MD5_DIGEST_SIZE; i++) {
		sprintf(sig_out + i * 2, "%02x", digest[i]);
	}
	sig_out[32] = '\0';
}

/**
 * Build the POST body for an award achievement request.
 * Returns allocated string (caller must free), or NULL on error.
 */
static char* sync_build_post_data(const char* username,
                                  const char* token,
                                  uint32_t achievement_id,
                                  uint8_t hardcore,
                                  const char* game_hash,
                                  uint32_t seconds_since) {
	char sig[33];
	sync_compute_signature(achievement_id, username, hardcore, seconds_since, sig);

	char* enc_username = HTTP_urlEncode(username);
	char* enc_token = HTTP_urlEncode(token);
	if (!enc_username || !enc_token) {
		free(enc_username);
		free(enc_token);
		return NULL;
	}

	/* r=awardachievement&u=<user>&t=<token>&a=<id>&h=<0|1>&v=<sig>[&m=<hash>][&o=<secs>] */
	size_t buf_size = 512 + strlen(enc_username) + strlen(enc_token);
	char* post_data = (char*)malloc(buf_size);
	if (!post_data) {
		free(enc_username);
		free(enc_token);
		return NULL;
	}

	int written = snprintf(post_data, buf_size,
	                       "r=awardachievement&u=%s&t=%s&a=%u&h=%u&v=%s",
	                       enc_username, enc_token, achievement_id,
	                       hardcore ? 1 : 0, sig);

	if (game_hash && game_hash[0] != '\0') {
		written += snprintf(post_data + written, buf_size - written,
		                    "&m=%s", game_hash);
	}
	if (seconds_since > 0) {
		written += snprintf(post_data + written, buf_size - written,
		                    "&o=%u", seconds_since);
	}

	free(enc_username);
	free(enc_token);
	return post_data;
}

/**
 * Parse the RA server response to determine success/failure.
 * 
 * Success: response contains "Success":true (with or without space after colon)
 * Already unlocked: response contains "User already has" — treated as success
 * 
 * Returns: 1 = success, 0 = server rejection (skip), -1 = network/parse error
 */
static int sync_parse_response(HTTP_Response* resp) {
	if (!resp || resp->http_status != 200 || !resp->data || resp->size == 0) {
		return -1;
	}

	/* Check for success: look for "Success":true (with or without space) */
	if (strstr(resp->data, "\"Success\":true") != NULL ||
	    strstr(resp->data, "\"Success\": true") != NULL) {
		return 1;
	}

	/* Check for "already has" — treat as success */
	if (strstr(resp->data, "User already has") != NULL) {
		return 1;
	}

	/* Server returned 200 but achievement was rejected */
	return 0;
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

bool RA_Sync_hasPendingUnlocks(uint32_t* out_count) {
	sync_ensure_init();

	RA_PendingUnlock* unlocks = NULL;
	uint32_t count = 0;

	if (!RA_Offline_ledgerGetPendingUnlocks(&unlocks, &count)) {
		if (out_count) *out_count = 0;
		return false;
	}

	free(unlocks);
	if (out_count) *out_count = count;
	return count > 0;
}

RA_SyncResult RA_Sync_syncAll(uint32_t game_id,
                              const RA_SyncConfig* config,
                              volatile bool* cancel,
                              RA_SyncProgressCallback progress_cb,
                              void* userdata) {
	RA_SyncResult result = {0, 0, 0, 0};

	sync_ensure_init();

	/* Use default config if none provided */
	if (!config) {
		config = &sync_default_config;
	}

	/* Seed RNG for random delays */
	srand((unsigned)time(NULL));

	/* Get credentials */
	const char* username = CFG_getRAUsername();
	const char* token = CFG_getRAToken();

	if (!username || !token || strlen(username) == 0 || strlen(token) == 0) {
		return result;
	}

	/* Read pending unlocks */
	RA_PendingUnlock* all_unlocks = NULL;
	uint32_t all_count = 0;

	if (!RA_Offline_ledgerGetPendingUnlocks(&all_unlocks, &all_count) || all_count == 0) {
		free(all_unlocks);
		return result;
	}

	/*
	 * If game_id is set, filter to only unlocks for that game.
	 * Otherwise process all pending unlocks.
	 */
	RA_PendingUnlock* unlocks = all_unlocks;
	uint32_t count = all_count;

	RA_PendingUnlock* filtered = NULL;
	if (game_id != 0) {
		/* Count matching unlocks first */
		uint32_t match_count = 0;
		for (uint32_t i = 0; i < all_count; i++) {
			if (all_unlocks[i].game_id == game_id) {
				match_count++;
			}
		}

		if (match_count == 0) {
			free(all_unlocks);
			return result;
		}

		/* Build filtered array */
		filtered = (RA_PendingUnlock*)malloc(match_count * sizeof(RA_PendingUnlock));
		if (!filtered) {
			free(all_unlocks);
			return result;
		}

		uint32_t j = 0;
		for (uint32_t i = 0; i < all_count; i++) {
			if (all_unlocks[i].game_id == game_id) {
				filtered[j++] = all_unlocks[i];
			}
		}

		free(all_unlocks);
		unlocks = filtered;
		count = match_count;
	}

	result.total = count;

	SYNC_LOG_INFO("Starting sync: %u pending unlocks, game_id=%u, time_now=%lld\n",
	              count, game_id, (long long)time(NULL));

	/* Process each pending unlock */
	for (uint32_t i = 0; i < count; i++) {
		/* Check cancel before starting each submission */
		if (cancel && *cancel) {
			break;
		}

		RA_PendingUnlock* unlock = &unlocks[i];

		/* Skip hardcore (should already be filtered, but be safe) */
		if (unlock->hardcore) {
			result.skipped++;
			if (progress_cb) {
				progress_cb(i + 1, count, false, userdata);
			}
			continue;
		}

		/* Compute seconds since unlock */
		time_t now = time(NULL);
		uint32_t seconds_since = 0;
		if ((time_t)unlock->timestamp < now) {
			seconds_since = (uint32_t)(now - (time_t)unlock->timestamp);
		}

		/* Diagnostic logging: capture all values used in the seconds_since
		 * computation so we can identify clock drift / timezone warp issues.
		 * If timestamp >= now, seconds_since stays 0 and &o= is omitted,
		 * causing the server to use its own time instead of the unlock time. */
		SYNC_LOG_INFO("ach=%u: ledger_timestamp=%u time_now=%lld diff=%lld seconds_since=%u%s\n",
		              unlock->achievement_id,
		              unlock->timestamp,
		              (long long)now,
		              (long long)(now - (time_t)unlock->timestamp),
		              seconds_since,
		              seconds_since == 0 ? " [WARNING: &o= will be omitted, server uses own time]" : "");

		/* Build request */
		char* post_data = sync_build_post_data(username, token,
		                                       unlock->achievement_id,
		                                       0, /* softcore */
		                                       unlock->game_hash,
		                                       seconds_since);
		if (!post_data) {
			SYNC_LOG_ERROR("ach=%u: failed to build POST data\n", unlock->achievement_id);
			result.skipped++;
			if (progress_cb) {
				progress_cb(i + 1, count, false, userdata);
			}
			continue;
		}

		/* Log the POST body for diagnostics.  Redact the token (&t=...) to
		 * avoid leaking credentials, but keep everything else so we can
		 * verify &o= is present and the signature is correct. */
		{
			/* Find &t= and the next & after it */
			const char* t_start = strstr(post_data, "&t=");
			if (t_start) {
				const char* t_end = strchr(t_start + 3, '&');
				SYNC_LOG_INFO("ach=%u: POST %.*s&t=***%s\n",
				              unlock->achievement_id,
				              (int)(t_start - post_data), post_data,
				              t_end ? t_end : "");
			} else {
				SYNC_LOG_INFO("ach=%u: POST %s\n",
				              unlock->achievement_id, post_data);
			}
		}

		/* Submit to server */
		HTTP_Response* http_resp = HTTP_post(RA_API_URL, post_data, NULL);
		free(post_data);

		/* Log raw server response before parsing */
		if (http_resp && http_resp->data && http_resp->size > 0) {
			/* Truncate at 512 chars to avoid flooding logs */
			int log_len = (int)http_resp->size;
			if (log_len > 512) log_len = 512;
			SYNC_LOG_INFO("ach=%u: server response (status=%d): %.*s\n",
			              unlock->achievement_id,
			              http_resp->http_status,
			              log_len, http_resp->data);
		} else if (http_resp) {
			SYNC_LOG_WARN("ach=%u: server response empty (status=%d)\n",
			              unlock->achievement_id, http_resp->http_status);
		} else {
			SYNC_LOG_WARN("ach=%u: no HTTP response (network failure?)\n",
			              unlock->achievement_id);
		}

		int parse_result = sync_parse_response(http_resp);
		if (http_resp) HTTP_freeResponse(http_resp);

		if (parse_result == 1) {
			/* Success — write SYNC_ACK to ledger */
			SYNC_LOG_INFO("ach=%u: server accepted (seconds_since=%u)\n",
			              unlock->achievement_id, seconds_since);
			RA_Offline_ledgerWriteSyncAck(unlock->achievement_id, unlock->game_id);
			/* Patch cached startsession to include this unlock so the next
			   offline-first launch doesn't re-trigger it */
			RA_Offline_patchStartsessionCacheWithUnlock(
				unlock->game_hash, unlock->achievement_id, unlock->timestamp);
			result.synced++;
		} else if (parse_result == 0) {
			/* Server rejected — skip and continue */
			SYNC_LOG_WARN("ach=%u: server rejected\n", unlock->achievement_id);
			result.skipped++;
		} else {
			/* Network error — stop sync, remaining unlocks stay pending */
			SYNC_LOG_ERROR("ach=%u: network error, stopping sync\n", unlock->achievement_id);
			result.failed++;
			if (progress_cb) {
				progress_cb(i + 1, count, false, userdata);
			}
			break;
		}

		if (progress_cb) {
			progress_cb(i + 1, count, parse_result == 1, userdata);
		}

		/* Delay before next submission (skip after last item or if cancelled) */
		if (i + 1 < count) {
			uint32_t delay;
			if ((i + 1) % config->delay_long_every == 0) {
				delay = sync_random_delay(config->delay_long_min_ms,
				                          config->delay_long_max_ms);
			} else {
				delay = sync_random_delay(config->delay_min_ms,
				                          config->delay_max_ms);
			}
			if (sync_interruptible_sleep(delay, cancel)) {
				break;  /* Cancelled during delay */
			}
		}
	}

	free(unlocks);

	/* Post-sync ledger maintenance */
	if (result.failed == 0 && (!cancel || !*cancel)) {
		/* Full sync completed — compact ledger to remove synced records */
		if (result.synced > 0) {
			RA_Offline_clearPendingCache();
			RA_Offline_ledgerCompact();
		}
	} else {
		/* Partial sync (failure or cancel) — refresh cache to reflect
		   what's still pending. Synced records have SYNC_ACK written
		   and will be cleaned up on next full sync or compaction. */
		if (result.synced > 0) {
			RA_Offline_refreshPendingCache();
		}
	}

	return result;
}

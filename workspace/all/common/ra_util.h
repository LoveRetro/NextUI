/**
 * ra_util.h — Shared inline utilities for the RetroAchievements subsystem.
 *
 * Header-only: every function is `static inline` so each translation unit
 * gets its own copy without needing a .c file or makefile changes.
 *
 * Contents:
 *   JSON parsing   — ra_find_json_bool(), ra_find_json_string()
 *   URL parameters — ra_extract_param()
 *   Directory I/O  — ra_mkdirs()
 *   Login POST     — ra_build_login_post()
 *   Sleep          — ra_interruptible_sleep()
 */

#ifndef RA_UTIL_H
#define RA_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "http.h"

/* SDL is only needed for the interruptible sleep helper.
 * Guard the include so pure-C callers that don't need sleep can still
 * include this header without pulling in SDL. */
#ifdef RA_UTIL_NEED_SDL
#include <SDL2/SDL.h>
#endif

/*****************************************************************************
 * JSON parsing helpers (DU-3)
 *
 * Minimal ad-hoc JSON helpers for RA API responses.  Handles both
 * compact ("key":value) and spaced ("key": value) formatting.
 *****************************************************************************/

/**
 * Search for a boolean value in JSON by key name.
 * Returns 1 for true, 0 for false, -1 if not found.
 */
static inline int ra_find_json_bool(const char* json, const char* key) {
	if (!json || !key) return -1;

	char search_true[128];
	char search_false[128];

	/* Try compact format first: "key":true / "key":false */
	snprintf(search_true, sizeof(search_true), "\"%s\":true", key);
	snprintf(search_false, sizeof(search_false), "\"%s\":false", key);

	if (strstr(json, search_true)) return 1;
	if (strstr(json, search_false)) return 0;

	/* Try with space after colon: "key": true / "key": false */
	snprintf(search_true, sizeof(search_true), "\"%s\": true", key);
	snprintf(search_false, sizeof(search_false), "\"%s\": false", key);

	if (strstr(json, search_true)) return 1;
	if (strstr(json, search_false)) return 0;

	return -1;
}

/**
 * Extract a quoted string value from JSON by key name.
 * Writes into out (at most out_size-1 chars + NUL).
 * Returns out on success, NULL if the key is not found.
 */
static inline const char* ra_find_json_string(const char* json, const char* key,
                                              char* out, size_t out_size) {
	if (!json || !key || !out || out_size == 0) return NULL;

	char search[128];

	/* Try compact: "key":"value" */
	snprintf(search, sizeof(search), "\"%s\":\"", key);
	const char* start = strstr(json, search);
	if (!start) {
		/* Try spaced: "key": "value" */
		snprintf(search, sizeof(search), "\"%s\": \"", key);
		start = strstr(json, search);
		if (!start) return NULL;
	}

	start += strlen(search);
	const char* end = strchr(start, '"');
	if (!end) return NULL;

	size_t len = (size_t)(end - start);
	if (len >= out_size) len = out_size - 1;

	memcpy(out, start, len);
	out[len] = '\0';

	return out;
}

/*****************************************************************************
 * URL parameter extraction (DU-5)
 *
 * Extract key=value pairs from URL query strings or POST bodies.
 *****************************************************************************/

/**
 * Extract the value of parameter @key from a URL query string or POST body.
 * Writes into buf (at most buf_size-1 chars + NUL).
 * Returns true if the parameter was found.
 */
static inline bool ra_extract_param(const char* str, const char* key,
                                    char* buf, size_t buf_size) {
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
		/* Check for '=' immediately after key */
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

/*****************************************************************************
 * Recursive directory creation (DU-7)
 *****************************************************************************/

/**
 * Recursively create directories for the given path (like `mkdir -p`).
 * Returns 0 on success, -1 on failure.
 */
static inline int ra_mkdirs(const char* path) {
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
 * Login POST builder (DU-6)
 *
 * Builds a URL-encoded "r=login&u=...&t=..." or "r=login&u=...&p=..." body.
 * The caller supplies a buffer; the function returns true on success.
 *****************************************************************************/

/**
 * Build a token-based login POST body with proper URL encoding.
 * Writes "r=login&u=<encoded_user>&t=<encoded_token>" into @buf.
 * Returns true on success, false on encoding failure.
 */
static inline bool ra_build_login_post_token(const char* username,
                                             const char* token,
                                             char* buf, size_t buf_size) {
	if (!username || !token || !buf || buf_size == 0) return false;

	char* enc_user = HTTP_urlEncode(username);
	char* enc_token = HTTP_urlEncode(token);
	if (!enc_user || !enc_token) {
		free(enc_user);
		free(enc_token);
		return false;
	}

	snprintf(buf, buf_size, "r=login&u=%s&t=%s", enc_user, enc_token);
	free(enc_user);
	free(enc_token);
	return true;
}

/**
 * Build a password-based login POST body with proper URL encoding.
 * Writes "r=login&u=<encoded_user>&p=<encoded_password>" into @buf.
 * Returns true on success, false on encoding failure.
 */
static inline bool ra_build_login_post_password(const char* username,
                                                const char* password,
                                                char* buf, size_t buf_size) {
	if (!username || !password || !buf || buf_size == 0) return false;

	char* enc_user = HTTP_urlEncode(username);
	char* enc_pass = HTTP_urlEncode(password);
	if (!enc_user || !enc_pass) {
		free(enc_user);
		free(enc_pass);
		return false;
	}

	snprintf(buf, buf_size, "r=login&u=%s&p=%s", enc_user, enc_pass);
	free(enc_user);
	free(enc_pass);
	return true;
}

/*****************************************************************************
 * Interruptible sleep (DU-4)
 *
 * Requires RA_UTIL_NEED_SDL to be defined before including this header.
 *****************************************************************************/

#ifdef RA_UTIL_NEED_SDL
/**
 * Sleep for @ms milliseconds, waking every 100ms to check *@cancel_flag.
 * Returns true if cancelled (flag became true), false if sleep completed.
 * @cancel_flag may be NULL (sleep runs to completion).
 */
static inline bool ra_interruptible_sleep(uint32_t ms, volatile bool* cancel_flag) {
	uint32_t elapsed = 0;
	while (elapsed < ms) {
		if (cancel_flag && *cancel_flag) return true;
		uint32_t chunk = (ms - elapsed > 100) ? 100 : (ms - elapsed);
		SDL_Delay(chunk);
		elapsed += chunk;
	}
	return (cancel_flag && *cancel_flag);
}
#endif /* RA_UTIL_NEED_SDL */

#endif /* RA_UTIL_H */

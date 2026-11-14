#include "http.h"
#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

// SDL is used here only for threading (SDL_CreateThread/SDL_DetachThread)
// to run HTTP requests asynchronously without blocking the main loop.
// HTTP communication itself uses curl via popen().
#include "sdl.h"

// Build version info (defined in makefile)
#ifndef BUILD_HASH
#define BUILD_HASH "dev"
#endif

// User agent string
#define HTTP_USER_AGENT_FMT "NextUI/%s (%s)"

/*****************************************************************************
 * Internal helpers
 *****************************************************************************/

// Buffer for reading curl output
typedef struct {
	char* data;
	size_t size;
	size_t capacity;
} HTTPBuffer;

static int HTTPBuffer_init(HTTPBuffer* buf) {
	buf->capacity = 4096;
	buf->data = malloc(buf->capacity);
	if (!buf->data) return -1;
	buf->data[0] = '\0';
	buf->size = 0;
	return 0;
}

static int HTTPBuffer_append(HTTPBuffer* buf, const char* data, size_t len) {
	if (buf->size + len + 1 > buf->capacity) {
		size_t new_cap = buf->capacity * 2;
		while (new_cap < buf->size + len + 1) {
			new_cap *= 2;
		}
		if (new_cap > HTTP_MAX_RESPONSE_SIZE) {
			return -1; // Too large
		}
		char* new_data = realloc(buf->data, new_cap);
		if (!new_data) return -1;
		buf->data = new_data;
		buf->capacity = new_cap;
	}
	memcpy(buf->data + buf->size, data, len);
	buf->size += len;
	buf->data[buf->size] = '\0';
	return 0;
}

static void HTTPBuffer_free(HTTPBuffer* buf) {
	if (buf->data) {
		free(buf->data);
		buf->data = NULL;
	}
	buf->size = 0;
	buf->capacity = 0;
}

// Escape a string for shell use (single quotes)
static char* shell_escape(const char* str) {
	if (!str) return strdup("");
	
	// Count how many single quotes we need to escape
	size_t len = strlen(str);
	size_t quotes = 0;
	for (size_t i = 0; i < len; i++) {
		if (str[i] == '\'') quotes++;
	}
	
	// Allocate: original + 3 chars per quote ('"'"') + 2 for surrounding quotes + 1 for null
	char* escaped = malloc(len + quotes * 3 + 3);
	if (!escaped) return NULL;
	
	char* p = escaped;
	*p++ = '\'';
	for (size_t i = 0; i < len; i++) {
		if (str[i] == '\'') {
			// End quote, escaped quote, start quote
			*p++ = '\'';
			*p++ = '"';
			*p++ = '\'';
			*p++ = '"';
			*p++ = '\'';
		} else {
			*p++ = str[i];
		}
	}
	*p++ = '\'';
	*p = '\0';
	
	return escaped;
}

// Execute curl and capture output
static HTTP_Response* execute_curl(const char* url, const char* post_data, const char* content_type) {
	HTTP_Response* response = calloc(1, sizeof(HTTP_Response));
	if (!response) return NULL;
	
	response->http_status = -1;
	
	// Build curl command
	// -s: silent (no progress)
	// -S: show errors
	// -k: insecure (skip SSL cert verification - needed on embedded devices without CA bundle)
	// -w '%{http_code}': write HTTP status at end
	// -o -: output to stdout
	// --connect-timeout: connection timeout
	// -m: max time
	// -L: follow redirects
	char cmd[4096];
	char user_agent[256];
	HTTP_getUserAgent(user_agent, sizeof(user_agent));
	
	char* escaped_url = shell_escape(url);
	char* escaped_ua = shell_escape(user_agent);
	
	if (!escaped_url || !escaped_ua) {
		free(escaped_url);
		free(escaped_ua);
		response->error = strdup("Memory allocation failed");
		return response;
	}
	
	if (post_data) {
		char* escaped_data = shell_escape(post_data);
		const char* ct = content_type ? content_type : "application/x-www-form-urlencoded";
		char* escaped_ct = shell_escape(ct);
		
		if (!escaped_data || !escaped_ct) {
			free(escaped_url);
			free(escaped_ua);
			free(escaped_data);
			free(escaped_ct);
			response->error = strdup("Memory allocation failed");
			return response;
		}
		
		snprintf(cmd, sizeof(cmd),
			"curl -s -S -k -L --connect-timeout %d -m %d "
			"-A %s "
			"-H 'Content-Type: %s' "
			"-d %s "
			"-w '\\n%%{http_code}' "
			"%s 2>&1",
			HTTP_TIMEOUT_SECS, HTTP_TIMEOUT_SECS * 2,
			escaped_ua,
			ct,
			escaped_data,
			escaped_url);
		
		free(escaped_data);
		free(escaped_ct);
	} else {
		snprintf(cmd, sizeof(cmd),
			"curl -s -S -k -L --connect-timeout %d -m %d "
			"-A %s "
			"-w '\\n%%{http_code}' "
			"%s 2>&1",
			HTTP_TIMEOUT_SECS, HTTP_TIMEOUT_SECS * 2,
			escaped_ua,
			escaped_url);
	}
	
	free(escaped_url);
	free(escaped_ua);
	
	// Execute curl
	FILE* pipe = popen(cmd, "r");
	if (!pipe) {
		response->error = strdup("Failed to execute curl");
		return response;
	}
	
	// Read output
	HTTPBuffer buf;
	if (HTTPBuffer_init(&buf) != 0) {
		pclose(pipe);
		response->error = strdup("Memory allocation failed");
		return response;
	}
	
	char read_buf[4096];
	size_t bytes_read;
	while ((bytes_read = fread(read_buf, 1, sizeof(read_buf), pipe)) > 0) {
		if (HTTPBuffer_append(&buf, read_buf, bytes_read) != 0) {
			HTTPBuffer_free(&buf);
			pclose(pipe);
			response->error = strdup("Response too large");
			return response;
		}
	}
	
	int exit_code = pclose(pipe);
	
	// Parse HTTP status from end of output
	// Output format: <body>\n<status_code>
	if (buf.size > 0) {
		// Find last newline
		char* last_newline = NULL;
		for (size_t i = buf.size; i > 0; i--) {
			if (buf.data[i-1] == '\n') {
				last_newline = &buf.data[i-1];
				break;
			}
		}
		
		if (last_newline) {
			// Parse status code after newline
			int status = atoi(last_newline + 1);
			if (status >= 100 && status < 600) {
				response->http_status = status;
				// Truncate buffer to remove status code
				*last_newline = '\0';
				buf.size = last_newline - buf.data;
			}
		}
	}
	
	// Check for curl errors
	if (exit_code != 0 && response->http_status <= 0) {
		// Curl failed - the output is likely an error message
		response->error = buf.data;
		buf.data = NULL;
		HTTPBuffer_free(&buf);
		return response;
	}
	
	// Success - transfer ownership of buffer
	response->data = buf.data;
	response->size = buf.size;
	buf.data = NULL; // Prevent free
	
	return response;
}

/*****************************************************************************
 * Async request handling
 *****************************************************************************/

typedef struct {
	char* url;
	char* post_data;
	char* content_type;
	HTTP_Callback callback;
	void* userdata;
} AsyncRequestData;

static int async_request_thread(void* data) {
	AsyncRequestData* req = (AsyncRequestData*)data;
	
	HTTP_Response* response;
	if (req->post_data) {
		response = execute_curl(req->url, req->post_data, req->content_type);
	} else {
		response = execute_curl(req->url, NULL, NULL);
	}
	
	// Call callback on completion
	if (req->callback) {
		req->callback(response, req->userdata);
	} else {
		HTTP_freeResponse(response);
	}
	
	// Cleanup request data
	free(req->url);
	free(req->post_data);
	free(req->content_type);
	free(req);
	
	return 0;
}

static void start_async_request(const char* url, const char* post_data, 
                                const char* content_type, HTTP_Callback callback, 
                                void* userdata) {
	AsyncRequestData* req = calloc(1, sizeof(AsyncRequestData));
	if (!req) {
		// Callback with error
		HTTP_Response* response = calloc(1, sizeof(HTTP_Response));
		if (response) {
			response->http_status = -1;
			response->error = strdup("Memory allocation failed");
		}
		if (callback) callback(response, userdata);
		return;
	}
	
	req->url = strdup(url);
	req->post_data = post_data ? strdup(post_data) : NULL;
	req->content_type = content_type ? strdup(content_type) : NULL;
	req->callback = callback;
	req->userdata = userdata;
	
	if (!req->url) {
		free(req->post_data);
		free(req->content_type);
		free(req);
		HTTP_Response* response = calloc(1, sizeof(HTTP_Response));
		if (response) {
			response->http_status = -1;
			response->error = strdup("Memory allocation failed");
		}
		if (callback) callback(response, userdata);
		return;
	}
	
	// Create thread
	SDL_Thread* thread = SDL_CreateThread(async_request_thread, "HTTPRequest", req);
	if (!thread) {
		free(req->url);
		free(req->post_data);
		free(req->content_type);
		free(req);
		HTTP_Response* response = calloc(1, sizeof(HTTP_Response));
		if (response) {
			response->http_status = -1;
			response->error = strdup("Failed to create thread");
		}
		if (callback) callback(response, userdata);
		return;
	}
	
	// Detach thread - it will clean itself up
	SDL_DetachThread(thread);
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

HTTP_Response* HTTP_get(const char* url) {
	return execute_curl(url, NULL, NULL);
}

HTTP_Response* HTTP_post(const char* url, const char* post_data, const char* content_type) {
	return execute_curl(url, post_data, content_type);
}

void HTTP_getAsync(const char* url, HTTP_Callback callback, void* userdata) {
	start_async_request(url, NULL, NULL, callback, userdata);
}

void HTTP_postAsync(const char* url, const char* post_data, const char* content_type,
                    HTTP_Callback callback, void* userdata) {
	start_async_request(url, post_data, content_type, callback, userdata);
}

void HTTP_freeResponse(HTTP_Response* response) {
	if (!response) return;
	free(response->data);
	free(response->error);
	free(response);
}

char* HTTP_urlEncode(const char* str) {
	if (!str) return NULL;
	
	// Count required size (worst case: every char becomes %XX)
	size_t len = strlen(str);
	char* encoded = malloc(len * 3 + 1);
	if (!encoded) return NULL;
	
	char* p = encoded;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)str[i];
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			*p++ = c;
		} else if (c == ' ') {
			*p++ = '+';
		} else {
			sprintf(p, "%%%02X", c);
			p += 3;
		}
	}
	*p = '\0';
	
	return encoded;
}

void HTTP_getUserAgent(char* buffer, size_t buffer_size) {
	snprintf(buffer, buffer_size, HTTP_USER_AGENT_FMT, BUILD_HASH, PLATFORM);
}

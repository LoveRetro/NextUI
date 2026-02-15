#include "ra_auth.h"
#include "http.h"
#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RetroAchievements API endpoints
#define RA_API_URL "https://retroachievements.org/dorequest.php"

// Minimal JSON helpers for RA login responses

static const char* find_json_string(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return NULL;
    
    // Search for "key":"value" pattern
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    
    const char* start = strstr(json, search);
    if (!start) {
        // Try "key": "value" (with space)
        snprintf(search, sizeof(search), "\"%s\": \"", key);
        start = strstr(json, search);
        if (!start) return NULL;
    }
    
    start += strlen(search);
    const char* end = strchr(start, '"');
    if (!end) return NULL;
    
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    
    strncpy(out, start, len);
    out[len] = '\0';
    
    return out;
}

static int find_json_bool(const char* json, const char* key) {
    if (!json || !key) return -1;
    
    // Search for "key":true or "key":false
    char search_true[128];
    char search_false[128];
    snprintf(search_true, sizeof(search_true), "\"%s\":true", key);
    snprintf(search_false, sizeof(search_false), "\"%s\":false", key);
    
    if (strstr(json, search_true)) return 1;
    if (strstr(json, search_false)) return 0;
    
    // Try with space after colon
    snprintf(search_true, sizeof(search_true), "\"%s\": true", key);
    snprintf(search_false, sizeof(search_false), "\"%s\": false", key);
    
    if (strstr(json, search_true)) return 1;
    if (strstr(json, search_false)) return 0;
    
    return -1;
}

// Parse RA login response
static void parse_login_response(const char* json, RA_AuthResponse* response) {
    if (!json || !response) return;
    
    // Check for Success field
    int success = find_json_bool(json, "Success");
    
    if (success == 1) {
        response->result = RA_AUTH_SUCCESS;
        
        // Extract Token
        find_json_string(json, "Token", response->token, sizeof(response->token));
        
        // Extract User (display name)
        find_json_string(json, "User", response->display_name, sizeof(response->display_name));
        
        if (strlen(response->token) == 0) {
            // Token missing in success response - shouldn't happen but handle it
            response->result = RA_AUTH_ERROR_PARSE;
            strncpy(response->error_message, "Token missing in response", 
                    sizeof(response->error_message) - 1);
        }
    } else if (success == 0) {
        response->result = RA_AUTH_ERROR_INVALID;
        
        // Try to extract error message
        if (!find_json_string(json, "Error", response->error_message, 
                             sizeof(response->error_message))) {
            strncpy(response->error_message, "Invalid credentials", 
                    sizeof(response->error_message) - 1);
        }
    } else {
        // Couldn't parse Success field
        response->result = RA_AUTH_ERROR_PARSE;
        strncpy(response->error_message, "Invalid response format", 
                sizeof(response->error_message) - 1);
    }
}

// Async authentication context
typedef struct {
    RA_AuthCallback callback;
    void* userdata;
} RA_AsyncAuthContext;

// HTTP callback for async auth
static void ra_auth_http_callback(HTTP_Response* http_response, void* userdata) {
    RA_AsyncAuthContext* ctx = (RA_AsyncAuthContext*)userdata;
    RA_AuthResponse response = {0};
    
    if (!http_response) {
        response.result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response.error_message, "No response received", 
                sizeof(response.error_message) - 1);
    } else if (http_response->error) {
        response.result = RA_AUTH_ERROR_NETWORK;
        strncpy(response.error_message, http_response->error, 
                sizeof(response.error_message) - 1);
    } else if (http_response->http_status != 200) {
        response.result = RA_AUTH_ERROR_NETWORK;
        snprintf(response.error_message, sizeof(response.error_message),
                 "HTTP error %d", http_response->http_status);
    } else if (!http_response->data || http_response->size == 0) {
        response.result = RA_AUTH_ERROR_PARSE;
        strncpy(response.error_message, "Empty response", 
                sizeof(response.error_message) - 1);
    } else {
        // Parse the JSON response
        parse_login_response(http_response->data, &response);
    }
    
    // Call the user's callback
    if (ctx->callback) {
        ctx->callback(&response, ctx->userdata);
    }
    
    // Cleanup
    HTTP_freeResponse(http_response);
    free(ctx);
}

void RA_authenticate(const char* username, const char* password,
                     RA_AuthCallback callback, void* userdata) {
    if (!username || !password) {
        RA_AuthResponse response = {0};
        response.result = RA_AUTH_ERROR_INVALID;
        strncpy(response.error_message, "Username and password required", 
                sizeof(response.error_message) - 1);
        if (callback) callback(&response, userdata);
        return;
    }
    
    // URL-encode username and password
    char* enc_username = HTTP_urlEncode(username);
    char* enc_password = HTTP_urlEncode(password);
    
    if (!enc_username || !enc_password) {
        free(enc_username);
        free(enc_password);
        RA_AuthResponse response = {0};
        response.result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response.error_message, "Memory allocation failed", 
                sizeof(response.error_message) - 1);
        if (callback) callback(&response, userdata);
        return;
    }
    
    // Build POST data: r=login&u=username&p=password
    char post_data[512];
    snprintf(post_data, sizeof(post_data), "r=login&u=%s&p=%s", 
             enc_username, enc_password);
    
    free(enc_username);
    free(enc_password);
    
    // Create async context
    RA_AsyncAuthContext* ctx = calloc(1, sizeof(RA_AsyncAuthContext));
    if (!ctx) {
        RA_AuthResponse response = {0};
        response.result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response.error_message, "Memory allocation failed", 
                sizeof(response.error_message) - 1);
        if (callback) callback(&response, userdata);
        return;
    }
    
    ctx->callback = callback;
    ctx->userdata = userdata;
    
    // Make async POST request
    HTTP_postAsync(RA_API_URL, post_data, NULL, ra_auth_http_callback, ctx);
}

RA_AuthResult RA_authenticateSync(const char* username, const char* password,
                                  RA_AuthResponse* response) {
    if (!response) return RA_AUTH_ERROR_UNKNOWN;
    memset(response, 0, sizeof(RA_AuthResponse));
    
    if (!username || !password) {
        response->result = RA_AUTH_ERROR_INVALID;
        strncpy(response->error_message, "Username and password required", 
                sizeof(response->error_message) - 1);
        return response->result;
    }
    
    // URL-encode username and password
    char* enc_username = HTTP_urlEncode(username);
    char* enc_password = HTTP_urlEncode(password);
    
    if (!enc_username || !enc_password) {
        free(enc_username);
        free(enc_password);
        response->result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response->error_message, "Memory allocation failed", 
                sizeof(response->error_message) - 1);
        return response->result;
    }
    
    // Build POST data
    char post_data[512];
    snprintf(post_data, sizeof(post_data), "r=login&u=%s&p=%s", 
             enc_username, enc_password);
    
    free(enc_username);
    free(enc_password);
    
    // Make synchronous POST request
    HTTP_Response* http_response = HTTP_post(RA_API_URL, post_data, NULL);
    
    if (!http_response) {
        response->result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response->error_message, "No response received", 
                sizeof(response->error_message) - 1);
        return response->result;
    }
    
    if (http_response->error) {
        response->result = RA_AUTH_ERROR_NETWORK;
        strncpy(response->error_message, http_response->error, 
                sizeof(response->error_message) - 1);
        HTTP_freeResponse(http_response);
        return response->result;
    }
    
    if (http_response->http_status != 200) {
        response->result = RA_AUTH_ERROR_NETWORK;
        snprintf(response->error_message, sizeof(response->error_message),
                 "HTTP error %d", http_response->http_status);
        HTTP_freeResponse(http_response);
        return response->result;
    }
    
    if (!http_response->data || http_response->size == 0) {
        response->result = RA_AUTH_ERROR_PARSE;
        strncpy(response->error_message, "Empty response", 
                sizeof(response->error_message) - 1);
        HTTP_freeResponse(http_response);
        return response->result;
    }
    
    // Parse the JSON response
    parse_login_response(http_response->data, response);
    
    HTTP_freeResponse(http_response);
    return response->result;
}

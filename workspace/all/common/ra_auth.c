#include "ra_auth.h"
#include "ra_util.h"
#include "http.h"
#include "config.h"
#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RetroAchievements API endpoints
#define RA_API_URL "https://retroachievements.org/dorequest.php"

// Parse RA login response
static void parse_login_response(const char* json, RA_AuthResponse* response) {
    if (!json || !response) return;
    
    // Check for Success field
    int success = ra_find_json_bool(json, "Success");
    
    if (success == 1) {
        response->result = RA_AUTH_SUCCESS;
        
        // Extract Token
        ra_find_json_string(json, "Token", response->token, sizeof(response->token));
        
        // Extract User (display name)
        ra_find_json_string(json, "User", response->display_name, sizeof(response->display_name));
        
        // Extract internal (server) username from AvatarUrl.
        // The RA server builds AvatarUrl from the internal username field
        // (e.g. "/UserPic/SammySwagz.png"), which may differ from the
        // display_name if the user has renamed their account.
        // Unfortunately, there is no current other way with the RA api to get the orginal 
        // username which was used, and if an offline achievement was send with an updated
        // name, the server will reject the unlock time, and use the current time instead.
        {
            char avatar_url[256] = {0};
            bool have_server_username = false;
            if (ra_find_json_string(json, "AvatarUrl", avatar_url, sizeof(avatar_url))) {
                have_server_username = CFG_setRAServerUsernameFromAvatarUrl(avatar_url);
            }
            if (!have_server_username) {
                // AvatarUrl missing or malformed — clear any stale value from a
                // prior login so offline sync falls back to CFG_getRAUsername().
                // This may produce incorrect unlock timestamps for renamed
                // accounts, but it's better than signing unlocks with a stale
                // internal username the server no longer recognizes.
                CFG_setRAServerUsername("");
            }
        }
        
        if (strlen(response->token) == 0) {
            // Token missing in success response - shouldn't happen but handle it
            response->result = RA_AUTH_ERROR_PARSE;
            strncpy(response->error_message, "Token missing in response", 
                    sizeof(response->error_message) - 1);
        }
    } else if (success == 0) {
        response->result = RA_AUTH_ERROR_INVALID;
        
        // Try to extract error message
        if (!ra_find_json_string(json, "Error", response->error_message, 
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
    
    // Build POST data: r=login&u=username&p=password
    char post_data[512];
    if (!ra_build_login_post_password(username, password, post_data, sizeof(post_data))) {
        RA_AuthResponse response = {0};
        response.result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response.error_message, "Failed to build login request", 
                sizeof(response.error_message) - 1);
        if (callback) callback(&response, userdata);
        return;
    }
    
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
    
    // Build POST data
    char post_data[512];
    if (!ra_build_login_post_password(username, password, post_data, sizeof(post_data))) {
        response->result = RA_AUTH_ERROR_UNKNOWN;
        strncpy(response->error_message, "Failed to build login request", 
                sizeof(response->error_message) - 1);
        return response->result;
    }
    
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

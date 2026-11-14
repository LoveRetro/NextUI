#ifndef __RA_AUTH_H__
#define __RA_AUTH_H__

// Handles RA login outside of rc_client for use before rc_client initialization.
// See: https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration#login

/**
 * Authentication result codes
 */
typedef enum {
    RA_AUTH_SUCCESS = 0,        // Authentication successful
    RA_AUTH_ERROR_NETWORK,      // Network/connection error
    RA_AUTH_ERROR_INVALID,      // Invalid credentials
    RA_AUTH_ERROR_PARSE,        // Failed to parse response
    RA_AUTH_ERROR_UNKNOWN       // Unknown error
} RA_AuthResult;

/**
 * Authentication response data
 */
typedef struct {
    RA_AuthResult result;
    char token[64];             // API token on success
    char display_name[64];      // Display name on success
    char error_message[256];    // Error message on failure
} RA_AuthResponse;

/**
 * Callback for async authentication requests.
 * @param response Authentication response (valid until callback returns)
 * @param userdata User-provided data passed to the auth request
 */
typedef void (*RA_AuthCallback)(const RA_AuthResponse* response, void* userdata);

/**
 * Authenticate with RetroAchievements using username and password.
 * This is an async operation - the callback will be called when complete.
 * 
 * @param username RetroAchievements username
 * @param password RetroAchievements password (will be URL-encoded)
 * @param callback Function to call with the result
 * @param userdata User data to pass to callback
 */
void RA_authenticate(const char* username, const char* password,
                     RA_AuthCallback callback, void* userdata);

/**
 * Synchronous authentication (blocks until complete).
 * Useful for simpler contexts where async isn't needed.
 * 
 * @param username RetroAchievements username
 * @param password RetroAchievements password
 * @param response Output: authentication response
 * @return RA_AuthResult code
 */
RA_AuthResult RA_authenticateSync(const char* username, const char* password,
                                  RA_AuthResponse* response);

#endif // __RA_AUTH_H__

/* ra_log.h — Parameterized RetroAchievements logging macros.
 *
 * Define RA_LOG_PREFIX to a string literal before including this header.
 *
 * Example:
 *   #define RA_LOG_PREFIX "RA_OFFLINE"
 *   #include "ra_log.h"
 *
 * RA_LOG_PREFIX is NOT undef'd here: the macros reference it at expansion
 * time (not definition time), so it must remain defined throughout the TU.
 */

#ifndef RA_LOG_PREFIX
#error "RA_LOG_PREFIX must be defined before including ra_log.h"
#endif

#define RA_LOG_DEBUG(...) LOG_debug("[" RA_LOG_PREFIX "] " __VA_ARGS__)
#define RA_LOG_INFO(...)  LOG_info("[" RA_LOG_PREFIX "] " __VA_ARGS__)
#define RA_LOG_WARN(...)  LOG_warn("[" RA_LOG_PREFIX "] " __VA_ARGS__)
#define RA_LOG_ERROR(...) LOG_error("[" RA_LOG_PREFIX "] " __VA_ARGS__)

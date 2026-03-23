#ifndef __RA_OFFLINE_H__
#define __RA_OFFLINE_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Offline RetroAchievements support for NextUI.
 * 
 * Provides:
 * - Response cache: Caches rcheevos server responses so games can load
 *   with achievements active when offline.
 * - Unlock ledger: Append-only binary log with hash chain integrity for
 *   persisting achievement unlocks across app restarts.
 * - Sync engine: Submits pending offline unlocks to the server with
 *   realistic timing when connectivity is restored.
 */

/* Cache directory under SHARED_USERDATA_PATH */
#define RA_OFFLINE_DIR  "/.ra/offline"
#define RA_CACHE_DIR    "/.ra/offline/cache"
#define RA_LEDGER_FILE  "/.ra/offline/ledger.bin"

/*****************************************************************************
 * Ledger record types
 *****************************************************************************/

typedef enum {
	RA_LEDGER_SESSION_START    = 0x01,
	RA_LEDGER_ACHIEVEMENT_UNLOCK = 0x02,
	RA_LEDGER_SESSION_END      = 0x03,
	RA_LEDGER_SYNC_ACK         = 0x04
} RA_LedgerRecordType;

/**
 * On-disk ledger record (packed, fixed size).
 * Hash chain: each record's prev_hash = SHA-256 of the previous record's
 * complete bytes (including its own record_hash).
 */
#pragma pack(push, 1)
typedef struct {
	uint8_t  type;              /* RA_LedgerRecordType */
	uint32_t timestamp;         /* Unix epoch seconds */
	uint32_t game_id;           /* RA game ID */
	uint32_t achievement_id;    /* 0 for session records */
	uint8_t  hardcore;          /* 0=softcore, 1=hardcore */
	char     game_hash[33];     /* Null-terminated MD5 hex */
	uint8_t  prev_hash[32];     /* SHA-256 of previous record (zeros for first) */
	uint8_t  record_hash[32];   /* SHA-256 of this record (bytes 0..prev_hash end) */
} RA_LedgerRecord;
#pragma pack(pop)

/* Size of record data that gets hashed to produce record_hash */
#define RA_LEDGER_RECORD_HASHABLE_SIZE  offsetof(RA_LedgerRecord, record_hash)

/* Pending unlock info returned by ledger query */
typedef struct {
	uint32_t achievement_id;
	uint32_t game_id;
	uint32_t timestamp;         /* Unix epoch of original unlock */
	char     game_hash[33];
	uint8_t  hardcore;
} RA_PendingUnlock;

/*****************************************************************************
 * Initialization / shutdown
 *****************************************************************************/

/**
 * Initialize the offline subsystem.
 * Creates directories, loads ledger state.
 * @param data_dir Base userdata path (SHARED_USERDATA_PATH)
 */
void RA_Offline_init(const char* data_dir);

/**
 * Shut down the offline subsystem.
 * Flushes any pending state.
 */
void RA_Offline_shutdown(void);

/*****************************************************************************
 * Offline mode state
 *****************************************************************************/

/**
 * Check if we are currently in offline mode.
 * @return true if offline (no network connectivity)
 */
bool RA_Offline_isOffline(void);

/**
 * Set the offline mode state.
 * Called during init based on WiFi state.
 */
void RA_Offline_setOffline(bool offline);

/*****************************************************************************
 * Response cache
 *****************************************************************************/

/**
 * Cache a server response to disk (write-through).
 * Determines cache key from URL/post_data parameters.
 * Only caches login and game data (patch) responses.
 * 
 * @param url Request URL
 * @param post_data POST body (may be NULL)
 * @param response_body Response body
 * @param response_len Response body length
 */
void RA_Offline_cacheResponse(const char* url, const char* post_data,
                              const char* response_body, size_t response_len);

/**
 * Retrieve a cached response from disk.
 * 
 * @param url Request URL
 * @param post_data POST body (may be NULL)
 * @param out_body Output: allocated response body (caller must free)
 * @param out_len Output: response body length
 * @return true if cache hit, false if miss or corrupt
 */
bool RA_Offline_getCachedResponse(const char* url, const char* post_data,
                                  char** out_body, size_t* out_len);

/*****************************************************************************
 * Unlock ledger
 *****************************************************************************/

/**
 * Write a SESSION_START record to the ledger.
 */
void RA_Offline_ledgerWriteSessionStart(uint32_t game_id, const char* game_hash,
                                        uint8_t hardcore);

/**
 * Write an ACHIEVEMENT_UNLOCK record to the ledger.
 */
void RA_Offline_ledgerWriteUnlock(uint32_t game_id, uint32_t achievement_id,
                                  const char* game_hash, uint8_t hardcore);

/**
 * Write a SESSION_END record to the ledger.
 */
void RA_Offline_ledgerWriteSessionEnd(uint32_t game_id, const char* game_hash);

/**
 * Write a SYNC_ACK record to the ledger, marking an unlock as server-confirmed.
 */
void RA_Offline_ledgerWriteSyncAck(uint32_t achievement_id, uint32_t game_id);

/**
 * Get all pending (unsynced) softcore unlocks from the ledger.
 * 
 * @param out_unlocks Output: allocated array (caller must free)
 * @param out_count Output: number of entries
 * @return true on success
 */
bool RA_Offline_ledgerGetPendingUnlocks(RA_PendingUnlock** out_unlocks,
                                        uint32_t* out_count);

/*****************************************************************************
 * Sync engine
 *****************************************************************************/

/**
 * Check if a sync is currently in progress.
 */
bool RA_Offline_isSyncing(void);

/**
 * Set the sync-in-progress flag.
 * Called by the sync engine in ra_integration.c.
 */
void RA_Offline_setSyncing(bool syncing);

#endif /* __RA_OFFLINE_H__ */

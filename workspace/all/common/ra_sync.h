#ifndef RA_SYNC_H
#define RA_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL_atomic.h>

/**
 * Standalone RetroAchievements offline sync engine.
 * 
 * Submits pending offline achievement unlocks to the RA server
 * without requiring the rcheevos library. Shared by both the
 * settings app (batch sync all games) and minarch (per-game sync
 * on game load).
 * 
 * Uses the existing ra_offline ledger and HTTP infrastructure.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Result of a sync operation.
 */
typedef struct {
	uint32_t synced;    /* Successfully synced to server */
	uint32_t skipped;   /* Skipped (server rejected, e.g. invalid achievement) */
	uint32_t failed;    /* Failed (network error, stops sync) */
	uint32_t total;     /* Total pending at start (after filtering) */
} RA_SyncResult;

/**
 * Configuration for sync timing.
 * 
 * Controls the delays between submissions. Use shorter delays when the
 * user is explicitly watching progress (settings), longer delays when
 * sync runs in the background during gameplay (minarch).
 */
typedef struct {
	uint32_t delay_min_ms;       /* Minimum delay between submissions */
	uint32_t delay_max_ms;       /* Maximum delay between submissions */
	uint32_t delay_long_min_ms;  /* Minimum long delay (every Nth submission) */
	uint32_t delay_long_max_ms;  /* Maximum long delay (every Nth submission) */
	uint32_t delay_long_every;   /* Apply long delay every N submissions */
} RA_SyncConfig;

/* Preset configs for common use cases */
#define RA_SYNC_CONFIG_INTERACTIVE \
	{ 500, 1500, 2000, 3000, 5 }     /* User is watching (settings) */
#define RA_SYNC_CONFIG_BACKGROUND \
	{ 2000, 5000, 5000, 10000, 5 }   /* Background during gameplay (minarch) */

/**
 * Progress callback invoked after each submission attempt.
 * Called from the sync thread (the calling thread).
 * 
 * @param current  1-based index of the unlock just processed
 * @param total    Total number of unlocks being processed
 * @param success  true if this unlock was synced successfully
 * @param userdata Opaque pointer passed to RA_Sync_syncAll
 */
typedef void (*RA_SyncProgressCallback)(uint32_t current, uint32_t total,
                                        bool success, void* userdata);

/**
 * Check if there are pending offline unlocks without performing any sync.
 * 
 * @param out_count Output: number of pending unlocks (may be NULL)
 * @return true if there are pending unlocks
 */
bool RA_Sync_hasPendingUnlocks(uint32_t* out_count);

/**
 * Sync pending offline unlocks to the RA server.
 * 
 * Blocks the calling thread. Processes one unlock at a time with
 * randomized delays between submissions to avoid rate limiting.
 * 
 * The cancel flag is polled between each submission. Setting it to
 * true will stop the sync after the current submission completes.
 * Already-synced unlocks are preserved (SYNC_ACK written to ledger).
 * 
 * @param game_id  Filter to a specific game (0 = sync all games).
 * @param config   Timing configuration. If NULL, uses interactive defaults.
 * @param cancel   Pointer to a cancel flag. Set to true to abort sync.
 *                 May be NULL if cancellation is not needed.
 * @param progress_cb Optional callback for progress updates.
 *                    May be NULL.
 * @param userdata User data passed to progress_cb.
 * @return RA_SyncResult with counts of synced/skipped/failed.
 */
RA_SyncResult RA_Sync_syncAll(uint32_t game_id,
                              const RA_SyncConfig* config,
                              SDL_atomic_t* cancel,
                              RA_SyncProgressCallback progress_cb,
                              void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* RA_SYNC_H */

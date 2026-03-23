# Offline RetroAchievements - Development Plan

Softcore offline achievement support for NextUI, following the approach approved by the RA team for melonDS Android.

## Design Principles

- **Softcore only** - hardcore is forced off when offline (no RA team pre-approval needed)
- **Write-through cache** - always re-fetch from server when online; cache is read-only fallback
- **No encryption** - open source project on open hardware; crypto is security theater
- **Hash-chain tamper evidence** - append-only ledger with SHA-256 chain for integrity
- **Complement rcheevos, don't fight it** - rcheevos handles mid-session retry with exponential backoff; we add persistence and offline startup

## Architecture

### Files

| File | Purpose |
|---|---|
| `../common/ra_offline.h` | Public API for offline subsystem |
| `../common/ra_offline.c` | Response cache, ledger, sync engine, SHA-256 |
| `ra_integration.c` | Hook points: `ra_server_call`, `ra_http_callback`, `ra_event_handler`, `RA_init` |
| `makefile` | Add `ra_offline.c` to SOURCE list |

### Data Paths

```
$(RA_DATA_DIR)/
  cache/
    login.bin          # Cached login response
    <game_hash>.bin    # Cached game data (patch) response
  ledger.bin           # Append-only achievement unlock log
```

---

## Phase 1: Response Cache Infrastructure

Cache rcheevos server responses to disk so they can be replayed when offline.

### Tasks

- [ ] **1.1** Implement SHA-256 (minimal, self-contained - no OpenSSL dependency)
  - Use a public domain / permissive implementation (e.g., Brad Conte's or similar)
  - Functions: `sha256_init`, `sha256_update`, `sha256_final`
- [ ] **1.2** Create `ra_offline.h` with public API
  - `RA_Offline_init(const char* data_dir)` - create dirs, load state
  - `RA_Offline_shutdown()` - flush, cleanup
  - `RA_Offline_cacheResponse(const char* url, const char* post_data, const char* response_body, size_t response_len)` - write-through cache
  - `RA_Offline_getCachedResponse(const char* url, const char* post_data, char** out_body, size_t* out_len)` - read cached response
  - `RA_Offline_isOffline()` - query current mode
- [ ] **1.3** Implement response cache in `ra_offline.c`
  - Parse `r=` parameter from URL/post_data to determine request type
  - Cache `login2` responses to `cache/login.bin`
  - Cache `patch` (game data) responses to `cache/<game_hash>.bin`
  - Binary format: `[uint32_t body_len][body_bytes][32-byte sha256]`
  - Validate SHA-256 on read; discard corrupt cache
- [ ] **1.4** Hook `ra_http_callback()` in `ra_integration.c`
  - After successful server response, call `RA_Offline_cacheResponse()`
  - Only cache responses with HTTP 200 and valid JSON body
- [ ] **1.5** Add `ra_offline.c` to minarch makefile SOURCE list
- [ ] **1.6** Build and verify cache writes occur during normal online play

---

## Phase 2: Offline Mode Startup

Remove the WiFi hard gate. When offline, serve cached responses to rcheevos.

### Tasks

- [ ] **2.1** Add offline state tracking to `ra_offline.c`
  - `static bool ra_offline_mode` flag
  - Set on init based on WiFi state; updated on network transitions
- [ ] **2.2** Modify `RA_init()` in `ra_integration.c`
  - Remove the early return when WiFi is unavailable
  - Always create `rc_client` and set up callbacks
  - If WiFi unavailable: set offline mode, force softcore, show notification
  - If WiFi available: proceed normally (online mode)
- [ ] **2.3** Modify `ra_server_call()` to intercept requests when offline
  - Check `RA_Offline_isOffline()`
  - For cacheable requests (`login2`, `patch`): serve cached response via callback
  - For non-cacheable requests (`startsession`, `ping`, `awardachievement`): return appropriate empty/success responses so rc_client proceeds
  - Call callback synchronously on the calling thread (no HTTP needed)
- [ ] **2.4** Handle `startsession` in offline mode
  - Return a minimal valid response so rc_client activates the game
  - Achievements will evaluate locally via cached definitions
- [ ] **2.5** Test offline startup flow
  - Verify game loads with achievements active when WiFi is off and cache exists
  - Verify graceful fallback (no crash, user notification) when WiFi is off and no cache exists
  - Verify online mode is unaffected

---

## Phase 3: Offline Unlock Ledger

Persist achievement unlocks to an append-only binary log with hash chain integrity.

### Tasks

- [ ] **3.1** Define ledger record format
  - Record types: `SESSION_START`, `ACHIEVEMENT_UNLOCK`, `SESSION_END`, `SYNC_ACK`
  - Fields per record:
    ```
    uint8_t  type
    uint32_t timestamp      (unix epoch)
    uint32_t game_id
    uint32_t achievement_id (0 for session records)
    uint8_t  hardcore       (0=softcore, 1=hardcore)
    char     game_hash[33]  (null-terminated md5)
    uint8_t  prev_hash[32]  (SHA-256 of previous record, zeros for first)
    uint8_t  record_hash[32] (SHA-256 of this record excluding this field)
    ```
- [ ] **3.2** Add ledger API to `ra_offline.h`
  - `RA_Offline_ledgerWriteSessionStart(uint32_t game_id, const char* game_hash, uint8_t hardcore)`
  - `RA_Offline_ledgerWriteUnlock(uint32_t game_id, uint32_t achievement_id, const char* game_hash, uint8_t hardcore)`
  - `RA_Offline_ledgerWriteSessionEnd(uint32_t game_id, const char* game_hash)`
  - `RA_Offline_ledgerWriteSyncAck(uint32_t achievement_id)` - marks entry as server-confirmed
  - `RA_Offline_ledgerGetPendingUnlocks(...)` - returns unsynced softcore unlocks
- [ ] **3.3** Implement ledger in `ra_offline.c`
  - Append-only file I/O (open, seek to end, write, fsync, close)
  - Hash chain: each record's `prev_hash` = SHA-256 of the previous record
  - Keep running hash in memory; recompute from file on init
  - Validate chain integrity on load; truncate at first corruption
- [ ] **3.4** Hook `ra_event_handler()` for `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`
  - **Always** write to ledger (both online and offline) as write-ahead log
  - This ensures unlocks survive app crash even when online retries are pending
- [ ] **3.5** Hook `ra_http_callback()` for successful `awardachievement` responses
  - Call `RA_Offline_ledgerWriteSyncAck()` to mark the unlock as server-confirmed
  - Prevents double-submission by the sync engine
- [ ] **3.6** Write `SESSION_START` when game loads, `SESSION_END` when game unloads
- [ ] **3.7** Test ledger integrity
  - Verify chain validation catches tampering
  - Verify recovery from truncated/corrupt ledger file

---

## Phase 4: Sync Engine

Submit pending offline unlocks to the server with realistic timing.

### Tasks

- [ ] **4.1** Add sync API to `ra_offline.h`
  - `RA_Offline_syncPending(rc_client_t* client)` - called when online at game load
  - `RA_Offline_isSyncing()` - query sync state
- [ ] **4.2** Implement sync engine in `ra_offline.c`
  - Read unsynced softcore unlocks from ledger
  - Discard hardcore unlocks (not submittable without RA team approval)
  - For each pending unlock:
    - Verify achievement definition still exists in current game data (skip if changed)
    - Submit via `rc_client` API or direct HTTP with `awardachievement` endpoint
    - Include accurate `seconds_since_unlock` from ledger timestamp
    - On success: write `SYNC_ACK` to ledger
  - Realistic timing between submissions:
    - 2-5s base delay between unlocks
    - 3-10s between groups
    - Jitter to avoid looking automated
    - Brief pause every ~5 unlocks
- [ ] **4.3** Hook sync into game load flow
  - After successful online login + game load, call `RA_Offline_syncPending()`
  - Run sync on a background thread to avoid blocking game start
  - Show notification: "Syncing X offline achievements..."
  - Show completion notification: "Synced X achievements" or "Sync complete"
- [ ] **4.4** Handle sync edge cases
  - Server rejects unlock (achievement not found, already unlocked): mark as synced, move on
  - Server is unreachable during sync: stop, retry next game load
  - Game hash mismatch (game was updated since offline play): skip those unlocks, log warning
- [ ] **4.5** Test sync flow end-to-end
  - Play offline, earn achievements, go online, verify sync submits correctly
  - Verify no double-submission of already-synced unlocks
  - Verify timing looks reasonable in server logs

---

## Phase 5: Hardcore Handling

Ensure hardcore mode is properly managed when offline.

### Tasks

- [ ] **5.1** Force softcore when offline
  - In `RA_init()` offline path: call `rc_client_set_hardcore_enabled(client, 0)`
  - Show notification: "Offline mode: softcore only"
- [ ] **5.2** Prevent hardcore toggle while offline
  - If user tries to enable hardcore while offline, deny and show notification
- [ ] **5.3** Record hardcore flag in ledger but don't sync
  - Ledger records whether hardcore was active (for future use if RA approves)
  - Sync engine skips entries where `hardcore == 1`
- [ ] **5.4** On transition to online: allow hardcore to be re-enabled
  - If user was in softcore due to offline, and WiFi returns, allow hardcore toggle
  - This is informational only — rcheevos handles the actual mode switch

---

## Key Integration Points in `ra_integration.c`

| Location | Hook | Purpose |
|---|---|---|
| `ra_server_call()` :619 | Intercept outbound requests | Serve cached responses when offline |
| `ra_http_callback()` :586 | Intercept inbound responses | Write-through cache; sync ACK |
| `ra_event_handler()` :662 | `ACHIEVEMENT_TRIGGERED` | Write unlock to ledger |
| `ra_event_handler()` :668 | Game load complete | Write `SESSION_START` to ledger |
| `RA_init()` :975 | Startup | Remove WiFi gate; init offline subsystem |
| `RA_quit()` | Shutdown | Write `SESSION_END`; flush ledger |

## rcheevos Built-in Behavior (do not reimplement)

- **Mid-session retry**: rc_client retries failed unlock submissions with exponential backoff (immediate -> 1s -> 2s -> ... -> 120s cap), indefinitely
- **Achievement evaluation**: Continues uninterrupted during disconnection — no network dependency
- **Disconnect/reconnect events**: Fired based on retry queue state — useful for UI only
- **Timestamp preservation**: Retried submissions include `seconds_since_unlock`
- **Our ledger complements this**: Survives app exit/crash; covers offline startup (where rc_client was never online)

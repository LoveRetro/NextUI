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

- [x] **1.1** Implement SHA-256 (minimal, self-contained - no OpenSSL dependency)
  - Use a public domain / permissive implementation (e.g., Brad Conte's or similar)
  - Functions: `sha256_init`, `sha256_update`, `sha256_final`
- [x] **1.2** Create `ra_offline.h` with public API
  - `RA_Offline_init(const char* data_dir)` - create dirs, load state
  - `RA_Offline_shutdown()` - flush, cleanup
  - `RA_Offline_cacheResponse(const char* url, const char* post_data, const char* response_body, size_t response_len)` - write-through cache
  - `RA_Offline_getCachedResponse(const char* url, const char* post_data, char** out_body, size_t* out_len)` - read cached response
  - `RA_Offline_isOffline()` - query current mode
- [x] **1.3** Implement response cache in `ra_offline.c`
  - Parse `r=` parameter from URL/post_data to determine request type
  - Cache `login2` responses to `cache/login.bin`
  - Cache `patch` (game data) responses to `cache/<game_hash>.bin`
  - Binary format: `[uint32_t body_len][body_bytes][32-byte sha256]`
  - Validate SHA-256 on read; discard corrupt cache
- [x] **1.4** Hook `ra_http_callback()` in `ra_integration.c`
  - After successful server response, call `RA_Offline_cacheResponse()`
  - Only cache responses with HTTP 200 and valid JSON body
- [x] **1.5** Add `ra_offline.c` to minarch makefile SOURCE list
- [x] **1.6** Build and verify cache writes occur during normal online play

---

## Phase 2: Offline Mode Startup

Remove the WiFi hard gate. When offline, serve cached responses to rcheevos.

### Tasks

- [x] **2.1** Add offline state tracking to `ra_offline.c`
  - `static bool ra_offline_mode` flag
  - Set on init based on WiFi state; updated on network transitions
- [x] **2.2** Modify `RA_init()` in `ra_integration.c`
  - Remove the early return when WiFi is unavailable
  - Always create `rc_client` and set up callbacks
  - If WiFi unavailable: set offline mode, force softcore, show notification
  - If WiFi available: proceed normally (online mode)
- [x] **2.3** Modify `ra_server_call()` to intercept requests when offline
  - Check `RA_Offline_isOffline()`
  - For cacheable requests (`login2`, `patch`): serve cached response via callback
  - For non-cacheable requests (`startsession`, `ping`, `awardachievement`): return appropriate empty/success responses so rc_client proceeds
  - Call callback synchronously on the calling thread (no HTTP needed)
- [x] **2.4** Handle `startsession` in offline mode
  - Return a minimal valid response so rc_client activates the game
  - Achievements will evaluate locally via cached definitions
- [x] **2.5** Test offline startup flow
  - Verify game loads with achievements active when WiFi is off and cache exists
  - Verify graceful fallback (no crash, user notification) when WiFi is off and no cache exists
  - Verify online mode is unaffected

---

## Phase 3: Offline Unlock Ledger

Persist achievement unlocks to an append-only binary log with hash chain integrity.

### Tasks

- [x] **3.1** Define ledger record format
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
- [x] **3.2** Add ledger API to `ra_offline.h`
  - `RA_Offline_ledgerWriteSessionStart(uint32_t game_id, const char* game_hash, uint8_t hardcore)`
  - `RA_Offline_ledgerWriteUnlock(uint32_t game_id, uint32_t achievement_id, const char* game_hash, uint8_t hardcore)`
  - `RA_Offline_ledgerWriteSessionEnd(uint32_t game_id, const char* game_hash)`
  - `RA_Offline_ledgerWriteSyncAck(uint32_t achievement_id)` - marks entry as server-confirmed
  - `RA_Offline_ledgerGetPendingUnlocks(...)` - returns unsynced softcore unlocks
- [x] **3.3** Implement ledger in `ra_offline.c`
  - Append-only file I/O (open, seek to end, write, fsync, close)
  - Hash chain: each record's `prev_hash` = SHA-256 of the previous record
  - Keep running hash in memory; recompute from file on init
  - Validate chain integrity on load; truncate at first corruption
- [x] **3.4** Hook `ra_event_handler()` for `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`
  - **Always** write to ledger (both online and offline) as write-ahead log
  - This ensures unlocks survive app crash even when online retries are pending
- [x] **3.5** Hook `ra_http_callback()` for successful `awardachievement` responses
  - Call `RA_Offline_ledgerWriteSyncAck()` to mark the unlock as server-confirmed
  - Prevents double-submission by the sync engine
- [x] **3.6** Write `SESSION_START` when game loads, `SESSION_END` when game unloads
- [x] **3.7** Test ledger integrity
  - Verify chain validation catches tampering
  - Verify recovery from truncated/corrupt ledger file

---

## Phase 4: Sync Engine

Submit pending offline unlocks to the server with realistic timing.

### Tasks

- [x] **4.1** Add sync API to `ra_offline.h`
  - `RA_Offline_syncPending(rc_client_t* client)` - called when online at game load
  - `RA_Offline_isSyncing()` - query sync state
- [x] **4.2** Implement sync engine in `ra_integration.c`
  - Read unsynced softcore unlocks from ledger
  - Discard hardcore unlocks (not submittable without RA team approval)
  - For each pending unlock:
    - Submit via rcheevos `rc_api_init_award_achievement_request` API
    - Include accurate `seconds_since_unlock` from ledger timestamp
    - On success: write `SYNC_ACK` to ledger
  - Realistic timing between submissions:
    - 2-5s base delay between unlocks
    - 5-10s pause every 5 unlocks
    - Random jitter via LCG PRNG
- [x] **4.3** Hook sync into game load flow
  - After successful online game load, call `ra_start_offline_sync()`
  - Run sync on a background (detached SDL) thread to avoid blocking game start
  - Show notification: "Syncing X offline achievements..."
  - Show completion notification: "Synced X achievements"
- [x] **4.4** Handle sync edge cases
  - Server rejects unlock (achievement not found, already unlocked): mark as synced, move on
  - Server is unreachable during sync: stop, retry next game load
  - Abort sync on game unload or app shutdown (interruptible sleep loop)
- [x] **4.5** Test sync flow end-to-end
  - Play offline, earn achievements, go online, verify sync submits correctly
  - Verify no double-submission of already-synced unlocks
  - Verify timing looks reasonable in server logs

---

## Phase 5: Hardcore Handling

Ensure hardcore mode is properly managed when offline.

### Tasks

- [x] **5.1** Force softcore when offline
  - In `RA_init()` offline path: call `rc_client_set_hardcore_enabled(client, 0)`
  - Show notification: "Offline mode: softcore only"
- [x] **5.2** Prevent hardcore toggle while offline
  - UI toggle is currently commented out in settings.cpp (hidden until RA team approval)
  - Runtime guard in `RA_init()` forces softcore when `RA_Offline_isOffline()` is true
- [x] **5.3** Record hardcore flag in ledger but don't sync
  - Ledger records whether hardcore was active (for future use if RA approves)
  - Sync engine skips entries where `hardcore == 1`
- [x] **5.4** On transition to online: allow hardcore to be re-enabled
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

---

## Phase 6: Production Hardening

Cleanup and robustness improvements for production use.

### Tasks

- [x] **6.1** Ledger compaction after successful sync
  - `RA_Offline_ledgerCompact()` removes synced UNLOCK+SYNC_ACK pairs and session records
  - Deletes ledger entirely when all unlocks are acked
  - Rewrites remaining records with rebuilt hash chain (atomic via temp file + rename)
  - Called from sync thread after `failed == 0`
- [x] **6.2** Resilient chain validation on load
  - `ledger_validate_and_load()` no longer truncates at first chain break
  - Records with valid self-hash are accepted even if chain link is broken
  - Corrupt records (bad record_hash) are skipped, subsequent records continue to be read
  - Chain repaired on next compaction
- [x] **6.3** Remove diagnostic logging
  - Removed all `DIAG:` prefixed log lines from `ra_offline.c` and `ra_integration.c`
  - Downgraded routine per-request logs from INFO to DEBUG
- [x] **6.4** Sanitize API token in error logs
  - HTTP error handler now logs only request type (`r=login2`) instead of full post_data
  - Prevents token leakage in log files
- [x] **6.5** Order-aware SYNC_ACK matching in ledger
  - `ledgerGetPendingUnlocks()` processes records sequentially; SYNC_ACK cancels earliest preceding UNLOCK only
  - `ledgerCompact()` uses same order-aware matching via scratch flag in prev_hash field
  - Fixes bug where re-unlocking a previously synced achievement was silently dropped

---

## Phase 7: Connectivity State Machine

Offline-first startup with async background connectivity probe. Seamless online/offline
transitions without blocking the user or losing unlocks.

### Design

```
State Machine:

                     RA_init()
                        │
           ┌────────────┴────────────┐
           │ cache exists?           │ no cache
           ▼                         ▼
    ┌──────────────┐          ┌──────────────┐
    │   OFFLINE    │          │  CONNECTING  │
    │  (from cache)│          │ (current     │
    │  probe runs  │          │  behavior)   │
    └──────┬───────┘          └──────────────┘
           │                         │
    probe succeeds              login succeeds
           │                         │
           ▼                         ▼
    ┌──────────────┐          ┌──────────────┐
    │    ONLINE    │◄─────────│    ONLINE    │
    │  probe stops │          │              │
    └──────┬───────┘          └──────────────┘
           │
    DISCONNECTED event
    (rcheevos detects failures)
           │
           ▼
    ┌──────────────┐
    │   OFFLINE    │
    │  probe runs  │──── probe succeeds ──► ONLINE
    └──────────────┘
```

**States:**

| State | `RA_Offline_isOffline()` | `ra_server_call` behavior | Probe thread |
|-------|--------------------------|---------------------------|-------------|
| OFFLINE | true | Serve from cache / synthetic | Running (30s interval) |
| ONLINE | false | Real HTTP requests | Stopped |
| CONNECTING | false | Real HTTP requests (may fail) | Not running |

**Transitions:**

- OFFLINE → ONLINE: probe login succeeds (or rcheevos RECONNECTED event)
- ONLINE → OFFLINE: rcheevos fires DISCONNECTED event
- CONNECTING → ONLINE: initial login succeeds (existing login retry logic)
- CONNECTING → failed: all login retries exhausted (no cache, no connectivity)

### Tasks

- [x] **7.1** Add connectivity probe state and constants
  - `RA_PROBE_INTERVAL_MS` (30000ms = 30s between probes)
  - `ra_probe_thread`, `ra_probe_abort` volatile flag
  - Deferred flags: `ra_deferred_online_notification`, `ra_deferred_sync`, `ra_deferred_hardcore_enable`
- [x] **7.2** Implement `ra_connectivity_probe_func()` background thread
  - Build login request via `rc_api_init_login_request()` (independent of rcheevos client)
  - `HTTP_post()` synchronous call (background thread, blocking OK)
  - On HTTP 200 with `"Success":true`: set `RA_Offline_setOffline(false)`, set deferred flags, exit
  - On failure: sleep 30s in 200ms chunks (checking abort flag), retry
  - No retry limit — probes until success or abort
  - Caches the successful login response (write-through)
- [x] **7.3** Implement `ra_start_connectivity_probe()` / `ra_stop_connectivity_probe()`
  - Start: guard against double-start, create detached SDL thread
  - Stop: set abort flag, wait up to 1s for thread exit
- [x] **7.4** Modify `RA_init()` for offline-first startup
  - Remove 3-second blocking WiFi wait loop
  - If WiFi enabled AND cached login exists: start offline, load from cache, launch probe
  - If WiFi enabled but NO cached login: current behavior (online login with retries)
  - If WiFi not enabled: pure offline mode (no probe)
- [x] **7.5** Modify `RA_idle()` for deferred state transitions
  - Check `ra_deferred_online_notification` → push "Connected" notification
  - Check `ra_deferred_sync` → call `ra_start_offline_sync()`
  - Check `ra_deferred_hardcore_enable` → re-enable hardcore if configured
- [x] **7.6** Modify `RC_CLIENT_EVENT_DISCONNECTED` handler
  - `RA_Offline_setOffline(true)` — flip to offline mode
  - Force softcore: `rc_client_set_hardcore_enabled(client, 0)`
  - Start connectivity probe
- [x] **7.7** Modify `RC_CLIENT_EVENT_RECONNECTED` handler
  - Stop connectivity probe (redundant — rcheevos confirmed connectivity)
  - `RA_Offline_setOffline(false)`
  - Re-enable hardcore if configured
  - Start offline sync
- [x] **7.8** Modify `RA_quit()` and `RA_unloadGame()`
  - Abort connectivity probe thread alongside sync thread abort
- [x] **7.9** Always update pending cache on achievement unlock
  - Remove `if (RA_Offline_isOffline())` guard on `RA_Offline_addPendingCacheEntry()`
  - Ensures `[O]` indicator is ready if disconnect occurs before server confirms
- [ ] **7.10** Test connectivity state machine
  - WiFi on + cache: instant offline load → probe connects → online
  - WiFi on + no cache: online login with retries (current behavior)
  - WiFi off: pure offline mode, no probe
  - WiFi drops mid-session: DISCONNECTED → offline + probe → ONLINE when WiFi returns
  - Hotspot with no actual connectivity: instant offline load, probe retries harmlessly

/**
 * ra_fsm.h — Event queue for the RetroAchievements state machine (SM-2).
 *
 * Background threads (connectivity probe, sync engine) post events into a
 * mutex-protected queue.  The main thread drains the queue during its
 * periodic processing loop (ra_process_deferred_flags) and asserts that the
 * events agree with the existing deferred-flag mechanism.
 *
 * This is a belt-and-suspenders validation phase: both deferred flags AND
 * the event queue carry the same information.  The main thread processes
 * flags as before and uses the events to cross-check.  No behavioral change
 * is introduced — events are purely additive.
 *
 * Lifecycle:
 *   RA_FSM_init()  — call once at startup (creates mutex + queue)
 *   RA_FSM_post()  — call from any thread (enqueues event under lock)
 *   RA_FSM_drain() — call from main thread (dequeues all pending events)
 *   RA_FSM_quit()  — call once at shutdown (drains queue, destroys mutex)
 */

#ifndef RA_FSM_H
#define RA_FSM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * Event types
 *
 * Each event corresponds to a specific deferred-flag transition that a
 * background thread sets.  The main thread uses these to cross-check
 * the snapshot it reads from the RADeferredState struct.
 *****************************************************************************/

typedef enum {
	/* ---- Connectivity probe thread ---- */

	/** Probe login succeeded — device is back online.
	 *  Corresponds to: offline_notification=false (cancel), online_notification=true,
	 *  sync=true, and optionally hardcore_enable=true. */
	RA_EV_PROBE_ONLINE,

	/** Probe thread exiting without achieving connectivity.
	 *  Corresponds to: ra_probe_running=false (on early-exit or abort). */
	RA_EV_PROBE_STOPPED,

	/* ---- Sync thread ---- */

	/** Sync completed with results ready for main thread.
	 *  Payload: sync_ids[], sync_timestamps[], sync_count.
	 *  Corresponds to: sync_apply=true + sync_ids/timestamps/count. */
	RA_EV_SYNC_DONE,

	/** Sync failed — device going back to offline mode.
	 *  Corresponds to: offline_notification=true, hardcore_disable=true,
	 *  user_saw_offline=true, and probe restart. */
	RA_EV_SYNC_FAILED,

	RA_EV_COUNT  /* sentinel — not a real event */
} RAEventType;

/*****************************************************************************
 * Event payload
 *
 * Events that carry data use a union.  Most events are "signal-only" (no
 * payload beyond the type).  RA_EV_SYNC_DONE carries the synced achievement
 * IDs and timestamps.  RA_EV_PROBE_ONLINE carries the hardcore_enable flag
 * to indicate whether hardcore re-enable was requested.
 *****************************************************************************/

#define RA_FSM_MAX_SYNC_IDS 256

typedef struct {
	RAEventType type;
	union {
		/** RA_EV_PROBE_ONLINE payload */
		struct {
			bool hardcore_enable;       /* true if probe set hardcore_enable */
			bool offline_notif_cancel;  /* true if pending offline notif was cancelled */
		} probe_online;

		/** RA_EV_SYNC_DONE payload */
		struct {
			uint32_t ids[RA_FSM_MAX_SYNC_IDS];
			uint32_t timestamps[RA_FSM_MAX_SYNC_IDS];
			uint32_t count;
		} sync_done;
	} data;
} RAEvent;

/*****************************************************************************
 * Queue API
 *****************************************************************************/

/** Maximum events that can be queued before the oldest is overwritten. */
#define RA_FSM_QUEUE_CAPACITY 32

/**
 * Initialize the event queue (create mutex, zero state).
 * Safe to call multiple times; subsequent calls are no-ops.
 */
void RA_FSM_init(void);

/**
 * Shut down the event queue (drain pending events, destroy mutex).
 * Safe to call if never initialized.
 */
void RA_FSM_quit(void);

/**
 * Post an event from any thread.  Thread-safe (locks internal mutex).
 * If the queue is full, the event is logged and dropped (should never
 * happen in practice — the queue is sized generously for the actual
 * event rate of ~4 events per probe/sync cycle).
 */
void RA_FSM_post(const RAEvent* event);

/**
 * Drain all pending events into @out_buf (up to @capacity entries).
 * Returns the number of events dequeued.  Main thread only.
 *
 * @param out_buf   Caller-provided array to receive events.
 * @param capacity  Size of out_buf.
 * @return          Number of events written to out_buf (0 if empty).
 */
uint32_t RA_FSM_drain(RAEvent* out_buf, uint32_t capacity);

/**
 * Convenience: post a signal-only event (no payload).
 * Equivalent to constructing an RAEvent with the given type and zeroed data.
 */
void RA_FSM_post_signal(RAEventType type);

#ifdef __cplusplus
}
#endif

#endif /* RA_FSM_H */

/**
 * ra_event_queue.h — Thread-safe event queue for RetroAchievements background threads.
 *
 * Background threads (connectivity probe, sync engine) post events into a
 * mutex-protected circular buffer.  The main thread drains the queue during
 * its periodic processing loop (ra_process_deferred_flags) and applies the
 * state transitions described by each event.
 *
 * Lifecycle:
 *   RA_EVQ_init()  — call once at startup (creates mutex + queue)
 *   RA_EVQ_post()  — call from any thread (enqueues event under lock)
 *   RA_EVQ_drain() — call from main thread (dequeues all pending events)
 *   RA_EVQ_quit()  — call once at shutdown (drains queue, destroys mutex)
 */

#ifndef RA_EVENT_QUEUE_H
#define RA_EVENT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * Event types
 *
 * Each event represents a state transition that a background thread needs
 * the main thread to apply.
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

#define RA_EVQ_MAX_SYNC_IDS 256

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
			uint32_t ids[RA_EVQ_MAX_SYNC_IDS];
			uint32_t timestamps[RA_EVQ_MAX_SYNC_IDS];
			uint32_t count;
		} sync_done;
	} data;
} RAEvent;

/*****************************************************************************
 * Queue API
 *****************************************************************************/

/** Maximum events that can be queued before the oldest is overwritten. */
#define RA_EVQ_QUEUE_CAPACITY 32

/**
 * Initialize the event queue (create mutex, zero state).
 * Safe to call multiple times; subsequent calls are no-ops.
 */
void RA_EVQ_init(void);

/**
 * Shut down the event queue (drain pending events, destroy mutex).
 * Safe to call if never initialized.
 */
void RA_EVQ_quit(void);

/**
 * Post an event from any thread.  Thread-safe (locks internal mutex).
 * If the queue is full, the event is logged and dropped (should never
 * happen in practice — the queue is sized generously for the actual
 * event rate of ~4 events per probe/sync cycle).
 */
void RA_EVQ_post(const RAEvent* event);

/**
 * Drain all pending events into @out_buf (up to @capacity entries).
 * Returns the number of events dequeued.  Main thread only.
 *
 * @param out_buf   Caller-provided array to receive events.
 * @param capacity  Size of out_buf.
 * @return          Number of events written to out_buf (0 if empty).
 */
uint32_t RA_EVQ_drain(RAEvent* out_buf, uint32_t capacity);

/**
 * Convenience: post a signal-only event (no payload).
 * Equivalent to constructing an RAEvent with the given type and zeroed data.
 */
void RA_EVQ_post_signal(RAEventType type);

#ifdef __cplusplus
}
#endif

#endif /* RA_EVENT_QUEUE_H */

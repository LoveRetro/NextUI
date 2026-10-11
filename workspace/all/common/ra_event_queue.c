/**
 * ra_event_queue.c — Thread-safe event queue implementation.
 *
 * A mutex-protected circular buffer.  Background threads push events,
 * the main thread drains them.  The queue is intentionally over-sized
 * (32 slots) relative to the actual event rate (~4 events per probe/sync
 * cycle) so overflow should never happen in practice.
 */

#include "ra_event_queue.h"

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

/* Logging — LOG_info/LOG_warn/LOG_debug are macros defined in api.h that
 * expand to LOG_note().  We don't include api.h here (too many transitive
 * deps) so declare LOG_note and define the macros we need locally.
 * Values must stay in sync with the LOG_* enum in api.h. */
enum { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2 };
void LOG_note(int level, const char* fmt, ...);
#define LOG_debug(fmt, ...) LOG_note(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_info(fmt, ...) LOG_note(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_warn(fmt, ...) LOG_note(LOG_WARN, fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Internal state
 *****************************************************************************/

static SDL_mutex* evq_mutex = NULL;
static RAEvent    evq_queue[RA_EVQ_QUEUE_CAPACITY];
static uint32_t   evq_head  = 0;   /* next slot to read  (main thread)    */
static uint32_t   evq_tail  = 0;   /* next slot to write (any thread)     */
static uint32_t   evq_count = 0;   /* current number of queued events     */

/*****************************************************************************
 * Diagnostic: event type -> string
 *****************************************************************************/

static const char* ra_event_type_str(RAEventType t) {
	switch (t) {
	case RA_EV_PROBE_ONLINE:  return "PROBE_ONLINE";
	case RA_EV_PROBE_STOPPED: return "PROBE_STOPPED";
	case RA_EV_SYNC_DONE:     return "SYNC_DONE";
	case RA_EV_SYNC_FAILED:   return "SYNC_FAILED";
	default:                  return "EV_?";
	}
}

/*****************************************************************************
 * Public API
 *****************************************************************************/

void RA_EVQ_init(void) {
	if (evq_mutex) return;  /* already initialized */
	evq_mutex = SDL_CreateMutex();
	evq_head  = 0;
	evq_tail  = 0;
	evq_count = 0;
	memset(evq_queue, 0, sizeof(evq_queue));
}

void RA_EVQ_quit(void) {
	if (!evq_mutex) return;
	/* Drain under lock to be tidy, then destroy */
	SDL_LockMutex(evq_mutex);
	evq_head  = 0;
	evq_tail  = 0;
	evq_count = 0;
	SDL_UnlockMutex(evq_mutex);

	SDL_DestroyMutex(evq_mutex);
	evq_mutex = NULL;
}

void RA_EVQ_post(const RAEvent* event) {
	if (!evq_mutex || !event) return;

	SDL_LockMutex(evq_mutex);

	if (evq_count >= RA_EVQ_QUEUE_CAPACITY) {
		LOG_warn("[RA_EVQ] Event queue full, dropping %s\n",
		         ra_event_type_str(event->type));
		SDL_UnlockMutex(evq_mutex);
		return;
	}

	evq_queue[evq_tail] = *event;
	evq_tail = (evq_tail + 1) % RA_EVQ_QUEUE_CAPACITY;
	evq_count++;

	LOG_debug("[RA_EVQ] Posted %s (queue depth %u)\n",
	         ra_event_type_str(event->type), evq_count);

	SDL_UnlockMutex(evq_mutex);
}

uint32_t RA_EVQ_drain(RAEvent* out_buf, uint32_t capacity) {
	if (!evq_mutex || !out_buf || capacity == 0) return 0;

	uint32_t n = 0;
	SDL_LockMutex(evq_mutex);

	while (evq_count > 0 && n < capacity) {
		out_buf[n] = evq_queue[evq_head];
		evq_head = (evq_head + 1) % RA_EVQ_QUEUE_CAPACITY;
		evq_count--;
		n++;
	}

	SDL_UnlockMutex(evq_mutex);
	return n;
}

void RA_EVQ_post_signal(RAEventType type) {
	RAEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	RA_EVQ_post(&ev);
}

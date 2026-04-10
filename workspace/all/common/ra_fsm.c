/**
 * ra_fsm.c — Event queue implementation for the RA state machine (SM-2).
 *
 * A simple mutex-protected circular buffer.  Background threads push events,
 * the main thread drains them.  The queue is intentionally over-sized
 * (32 slots) relative to the actual event rate (~4 events per probe/sync
 * cycle) so overflow should never happen in practice.
 *
 * Build note: this file must be compiled alongside the RA source files.
 * It requires SDL2 for the mutex.  The include path must contain
 * ../common/ (for ra_fsm.h) — this is already provided by the existing
 * makefile INCDIR.
 */

#include "ra_fsm.h"

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

/* Logging — LOG_info/LOG_warn are macros defined in api.h that expand to
 * LOG_note().  We don't include api.h here (too many transitive deps) so
 * declare LOG_note and define the two macros we need locally. */
enum { LOG_INFO = 1, LOG_WARN = 2 };
void LOG_note(int level, const char* fmt, ...);
#define LOG_info(fmt, ...) LOG_note(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_warn(fmt, ...) LOG_note(LOG_WARN, fmt, ##__VA_ARGS__)

/*****************************************************************************
 * Internal state
 *****************************************************************************/

static SDL_mutex* fsm_mutex = NULL;
static RAEvent    fsm_queue[RA_FSM_QUEUE_CAPACITY];
static uint32_t   fsm_head  = 0;   /* next slot to read  (main thread)    */
static uint32_t   fsm_tail  = 0;   /* next slot to write (any thread)     */
static uint32_t   fsm_count = 0;   /* current number of queued events     */

/*****************************************************************************
 * Diagnostic: event type → string
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

void RA_FSM_init(void) {
	if (fsm_mutex) return;  /* already initialized */
	fsm_mutex = SDL_CreateMutex();
	fsm_head  = 0;
	fsm_tail  = 0;
	fsm_count = 0;
	memset(fsm_queue, 0, sizeof(fsm_queue));
}

void RA_FSM_quit(void) {
	if (!fsm_mutex) return;
	/* Drain under lock to be tidy, then destroy */
	SDL_LockMutex(fsm_mutex);
	fsm_head  = 0;
	fsm_tail  = 0;
	fsm_count = 0;
	SDL_UnlockMutex(fsm_mutex);

	SDL_DestroyMutex(fsm_mutex);
	fsm_mutex = NULL;
}

void RA_FSM_post(const RAEvent* event) {
	if (!fsm_mutex || !event) return;

	SDL_LockMutex(fsm_mutex);

	if (fsm_count >= RA_FSM_QUEUE_CAPACITY) {
		/* Should never happen — log and drop */
		LOG_warn("[RA_FSM] Event queue full, dropping %s\n",
		         ra_event_type_str(event->type));
		SDL_UnlockMutex(fsm_mutex);
		return;
	}

	fsm_queue[fsm_tail] = *event;   /* struct copy */
	fsm_tail = (fsm_tail + 1) % RA_FSM_QUEUE_CAPACITY;
	fsm_count++;

	LOG_info("[RA_FSM] Posted %s (queue depth %u)\n",
	         ra_event_type_str(event->type), fsm_count);

	SDL_UnlockMutex(fsm_mutex);
}

uint32_t RA_FSM_drain(RAEvent* out_buf, uint32_t capacity) {
	if (!fsm_mutex || !out_buf || capacity == 0) return 0;

	uint32_t n = 0;
	SDL_LockMutex(fsm_mutex);

	while (fsm_count > 0 && n < capacity) {
		out_buf[n] = fsm_queue[fsm_head];   /* struct copy */
		fsm_head = (fsm_head + 1) % RA_FSM_QUEUE_CAPACITY;
		fsm_count--;
		n++;
	}

	SDL_UnlockMutex(fsm_mutex);
	return n;
}

void RA_FSM_post_signal(RAEventType type) {
	RAEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	RA_FSM_post(&ev);
}

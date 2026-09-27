#include <SDL2/SDL.h>
#include "ma_internal.h"
#include "ma_cheats.h"
#include "ma_rewind.h"
#include "ma_input.h"
#include "ma_config.h"
#include "ma_runframe.h"

void chooseSyncRef(void) {
	switch (sync_ref) {
		case SYNC_SRC_AUTO:   use_core_fps = (core.get_region() == RETRO_REGION_PAL); break;
		case SYNC_SRC_SCREEN: use_core_fps = 0; break;
		case SYNC_SRC_CORE:   use_core_fps = 1; break;
	}
	LOG_info("%s: sync_ref is set to %s, game region is %s, use core fps = %s\n",
		  __FUNCTION__,
		  sync_ref_labels[sync_ref],
		  core.get_region() == RETRO_REGION_NTSC ? "NTSC" : "PAL",
		  use_core_fps ? "yes" : "no");
}

#define MAX_FRAME_GAP_US 0x80000

static void limitFF(void) {
	static uint64_t ff_frame_time = 0;
	static uint64_t last_time = 0;
	static int last_max_speed = -1;
	if (last_max_speed!=max_ff_speed) {
		last_max_speed = max_ff_speed;
		ff_frame_time = 1000000 / (core.fps * (max_ff_speed + 1));
	}

	uint64_t now = getMicroseconds();
	if (fast_forward && max_ff_speed) {
		if (last_time == 0) last_time = now;
		int elapsed = now - last_time;
		if (elapsed>0 && elapsed<MAX_FRAME_GAP_US) {
			if (elapsed<ff_frame_time) {
				int delay = (ff_frame_time - elapsed) / 1000;
				if (delay>0 && delay<17) { // don't allow a delay any greater than a frame
					SDL_Delay(delay);
				}
			}
			last_time += ff_frame_time;
			return;
		}
	}
	last_time = now;
}

static retro_usec_t frame_time_reference(void) {
	if (core.frame_time_reference > 0) return core.frame_time_reference;
	if (core.fps > 0.0) return (retro_usec_t)(1000000.0 / core.fps);
	return (retro_usec_t)(1000000.0 / 60.0);
}

static void run_core(void) {
	if (core.frame_time_callback) {
		retro_usec_t usec;
		uint64_t now = getMicroseconds();
		if (fast_forward || rewinding) {
			usec = frame_time_reference();
		} else {
			usec = core.frame_time_last ? (retro_usec_t)(now - core.frame_time_last) : frame_time_reference();
			if (usec <= 0 || usec > MAX_FRAME_GAP_US) usec = frame_time_reference();
		}
		core.frame_time_last = now;
		core.frame_time_callback(usec);
	}
	core.run();
}

void run_frame(void) {
	Cheats_apply();

	// if rewind is toggled, fast-forward toggle must stay off; fast-forward hold pauses rewind
	int do_rewind = (rewind_pressed || rewind_toggle) && !(rewind_toggle && ff_hold_active);
	if (do_rewind) {
		int was_rewinding = rewinding;
		int rewind_result = Rewind_step_back();
		if (rewind_result == REWIND_STEP_OK) {
			// Actually stepped back - run one frame to render the restored state
			rewinding = 1;
			fast_forward = 0;
			run_core();
		}
		else if (rewind_result == REWIND_STEP_CADENCE) {
			// Waiting for cadence - don't run core, just re-render current frame
			rewinding = 1;
			fast_forward = 0;
			// Poll input manually since core.run() isn't called
			input_poll_callback();
			// Skip core.run() entirely to avoid advancing the game
		}
		else {
			int hold_empty = rewind_ctx.enabled && rewind_pressed && !rewind_toggle;
			if (hold_empty) {
				// Hold-to-rewind: freeze when empty to avoid advance/rewind oscillation.
				rewinding = was_rewinding ? 1 : 0;
				// Poll input manually so release is detected while core.run() is skipped
				input_poll_callback();
			} else {
				// Buffer empty: auto untoggle rewind, resume FF if it was paused for a hold
				if (rewind_toggle) rewind_toggle = 0;
				if (ff_paused_by_rewind_hold && ff_toggled) {
					ff_paused_by_rewind_hold = 0;
					fast_forward = setFastForward(1);
				}
				if (was_rewinding) {
					rewinding = 1;
					Rewind_sync_encode_state();
				}
				rewinding = 0;
				run_core();
				Rewind_push(0);
			}
		}
	}
	else {
		Rewind_sync_encode_state();
		rewinding = 0;
		if (ff_paused_by_rewind_hold && !rewind_pressed) {
			// resume fast forward after hold rewind ends
			if (ff_toggled) fast_forward = setFastForward(1);
			ff_paused_by_rewind_hold = 0;
		}

		run_core();
		Rewind_push(0);
	}
	limitFF();
}

#include <SDL2/SDL.h>
#include "minarch_internal.h"
#include "minarch_rewind.h"
#include "minarch_input.h"
#include "minarch_config.h"
#include "minarch_runframe.h"

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
		if (elapsed>0 && elapsed<0x80000) {
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

void run_frame(void) {
	// if rewind is toggled, fast-forward toggle must stay off; fast-forward hold pauses rewind
	int do_rewind = (rewind_pressed || rewind_toggle) && !(rewind_toggle && ff_hold_active);
	if (do_rewind) {
		int was_rewinding = rewinding;
		int rewind_result = Rewind_step_back();
		if (rewind_result == REWIND_STEP_OK) {
			// Actually stepped back - run one frame to render the restored state
			rewinding = 1;
			fast_forward = 0;
			core.run();
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
				core.run();
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

		core.run();
		Rewind_push(0);
	}
	limitFF();
}

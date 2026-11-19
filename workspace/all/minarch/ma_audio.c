#include <stdbool.h>
#include <SDL2/SDL.h>
#include <msettings.h>
#include "ma_internal.h"
#include "ma_audio.h"

static SDL_atomic_t resetAudio;

static void Audio_onSinkChanged(int device, int watch_event) {
	switch (watch_event) {
	case DIRWATCH_CREATE:      LOG_info("callback reason: DIRWATCH_CREATE\n");      break;
	case DIRWATCH_DELETE:      LOG_info("callback reason: DIRWATCH_DELETE\n");      break;
	case FILEWATCH_MODIFY:     LOG_info("callback reason: FILEWATCH_MODIFY\n");     break;
	case FILEWATCH_DELETE:     LOG_info("callback reason: FILEWATCH_DELETE\n");     break;
	case FILEWATCH_CLOSE_WRITE:LOG_info("callback reason: FILEWATCH_CLOSE_WRITE\n");break;
	}

	// FIXME: This shouldnt be necessary, alsa should just read .asoundrc for the changed default device.
	if (device == AUDIO_SINK_BLUETOOTH)
		SDL_setenv("AUDIODEV", "bluealsa", 1);
	else
		SDL_setenv("AUDIODEV", "default", 1);

	Audio_requestReset();
}

void Audio_init(void) {
	// load_game can report timing before the device exists, and SND_init already opens with it
	SDL_AtomicSet(&resetAudio, 0);
	SND_init(core.sample_rate, core.fps);
	SND_registerDeviceWatcher(Audio_onSinkChanged);
}

void Audio_requestReset(void) {
	SDL_AtomicSet(&resetAudio, 1);
}

void Audio_checkAndResetIfNeeded(void) {
	if (!SDL_AtomicSet(&resetAudio, 0)) return;
	LOG_info("Resetting audio (%g Hz, %g fps)\n", core.sample_rate, core.fps);
	SND_resetAudio(core.sample_rate, core.fps);
}

void audio_sample_callback(int16_t left, int16_t right) {
	if (rewinding && !rewind_ctx.audio) return;
	if (!fast_forward || ff_audio) {
		if (use_core_fps || fast_forward) {
			SND_batchSamples_fixed_rate(&(const SND_Frame){left,right}, 1);
		}
		else {
			SND_batchSamples(&(const SND_Frame){left,right}, 1);
		}
	}
}

size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
	if (rewinding && !rewind_ctx.audio) return frames;
	if (!fast_forward || ff_audio) {
		if (use_core_fps || fast_forward) {
			return SND_batchSamples_fixed_rate((const SND_Frame*)data, frames);
		}
		else {
			return SND_batchSamples((const SND_Frame*)data, frames);
		}
	}
	else return frames;
}

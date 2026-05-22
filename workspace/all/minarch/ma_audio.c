#include <stdbool.h>
#include <SDL2/SDL.h>
#include <msettings.h>
#include "ma_internal.h"
#include "ma_audio.h"

static bool resetAudio = false;

void Audio_onSinkChanged(int device, int watch_event) {
	switch (watch_event) {
	case DIRWATCH_CREATE:      LOG_info("callback reason: DIRWATCH_CREATE\n");      break;
	case DIRWATCH_DELETE:      LOG_info("callback reason: DIRWATCH_DELETE\n");      break;
	case FILEWATCH_MODIFY:     LOG_info("callback reason: FILEWATCH_MODIFY\n");     break;
	case FILEWATCH_DELETE:     LOG_info("callback reason: FILEWATCH_DELETE\n");     break;
	case FILEWATCH_CLOSE_WRITE:LOG_info("callback reason: FILEWATCH_CLOSE_WRITE\n");break;
	}

	SetAudioSink(device);
	sleep(1); // give it a moment to apply before we reset the audio system, otherwise

	
	resetAudio = true;
}

void Audio_checkAndResetIfNeeded(void) {
	if (!resetAudio) return;
	resetAudio = false;
	SetVolume(GetVolume());
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

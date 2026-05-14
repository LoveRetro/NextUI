#include "minarch_internal.h"
#include "minarch_audio.h"

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

#include <stdbool.h>
#include <string.h>
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

	resetAudio = true;

	// FIXME: This shouldnt be necessary, alsa should just read .asoundrc for the changed default device.
	if (device == AUDIO_SINK_BLUETOOTH)
		SDL_setenv("AUDIODEV", "bluealsa", 1);
	else
		SDL_setenv("AUDIODEV", "default", 1);
}

void Audio_checkAndResetIfNeeded(void) {
	if (!resetAudio) return;
	resetAudio = false;
	LOG_info("Resetting audio device config! (new state: %s)\n", SDL_getenv("AUDIODEV"));
	Audio_discard();
	SND_resetAudio(core.sample_rate, core.fps);
}

#define PENDING_FRAMES 4096
static SND_Frame pending[PENDING_FRAMES];
static size_t pending_count = 0;

static size_t mix(const SND_Frame *frames, size_t count) {
	if (use_core_fps || fast_forward) return SND_batchSamples_fixed_rate(frames, count);
	return SND_batchSamples(frames, count);
}

void Audio_flush(void) {
	if (!pending_count) return;
	size_t used = mix(pending, pending_count);
	pending_count -= used;
	if (used && pending_count) memmove(pending, pending + used, pending_count * sizeof(*pending));
}

void Audio_discard(void) {
	pending_count = 0;
}

size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
	if ((rewinding && !rewind_ctx.audio) || (fast_forward && !ff_audio)) return frames;
	if (!frames) return 0;
	// Cores such as Gambatte submit a stereo pair at a time through the batch API.
	if (frames == 1 && !fast_forward) {
		if (pending_count == PENDING_FRAMES) Audio_flush();
		if (pending_count == PENDING_FRAMES) return 0;
		pending[pending_count++] = (SND_Frame){data[0], data[1]};
		return 1;
	}
	Audio_flush();
	if (pending_count) return 0;
	return mix((const SND_Frame *)data, frames);
}

void audio_sample_callback(int16_t left, int16_t right) {
	// The void callback cannot retry. Drop this pair if both buffers are full.
	(void)audio_sample_batch_callback((const int16_t[]){left, right}, 1);
}

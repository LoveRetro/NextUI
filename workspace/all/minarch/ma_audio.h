#pragma once

#include "libretro.h"

void audio_sample_callback(int16_t left, int16_t right);
size_t audio_sample_batch_callback(const int16_t *data, size_t frames);
void Audio_onSinkChanged(int device, int watch_event);
void Audio_checkAndResetIfNeeded(void);
void Audio_flush(void);
void Audio_discard(void);

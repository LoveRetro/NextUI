#pragma once

#include "libretro.h"

void audio_sample_callback(int16_t left, int16_t right);
size_t audio_sample_batch_callback(const int16_t *data, size_t frames);
void Audio_init(void);
void Audio_requestReset(void);
void Audio_checkAndResetIfNeeded(void);

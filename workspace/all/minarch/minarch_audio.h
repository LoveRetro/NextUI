#pragma once

#include "libretro.h"

void audio_sample_callback(int16_t left, int16_t right);
size_t audio_sample_batch_callback(const int16_t *data, size_t frames);

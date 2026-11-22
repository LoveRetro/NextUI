#ifndef __CUBIC_RESAMPLER_H__
#define __CUBIC_RESAMPLER_H__

#include "defines.h"
#include "api.h"

void cubic_resampler_reset(void);
ResampledFrames cubic_resampler_process(const SND_Frame *input_frames, int input_frame_count,
										int input_sample_rate, int output_sample_rate, double final_ratio);

#endif

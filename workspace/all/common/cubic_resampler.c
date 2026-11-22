#include "cubic_resampler.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CUBIC_STACK_FRAMES 256

// Catmull-Rom resampler. pos is 32.32 fixed point so that splitting the
// input into chunks yields exactly the same output as one long call.
static struct
{
	SND_Frame history[3];
	uint64_t pos;
	int rate_in;
	int rate_out;
	struct {
		float gain, a1, a2;
		float z1[2], z2[2];
	} lowpass[2];
	double filtered_ratio;
} cubic;

void cubic_resampler_reset(void)
{
	memset(&cubic, 0, sizeof(cubic));
	cubic.pos = 1ULL << 32;
}

// A fourth-order Butterworth low-pass removes frequencies above the new Nyquist limit.
// Small clock corrections at a nominal 1:1 rate can stay on the unfiltered path.
static void cubic_filter(const SND_Frame *input, SND_Frame *output, int count, double ratio)
{
	ratio = fmin(ratio, 1.0);
	if (fabs(ratio - cubic.filtered_ratio) > 0.005 * ratio) {
		double k = tan(M_PI * 0.46 * ratio);
		const double damping[2] = {1.847759065, 0.765366865};
		for (int i = 0; i < 2; i++) {
			double norm = 1.0 / (1.0 + damping[i] * k + k * k);
			cubic.lowpass[i].gain = k * k * norm;
			cubic.lowpass[i].a1 = 2.0 * (k * k - 1.0) * norm;
			cubic.lowpass[i].a2 = (1.0 - damping[i] * k + k * k) * norm;
		}
		cubic.filtered_ratio = ratio;
	}
	for (int n = 0; n < count; n++) {
		float stereo[2] = {input[n].left, input[n].right};
		for (int i = 0; i < 2; i++) {
			for (int ch = 0; ch < 2; ch++) {
				float x = stereo[ch] * cubic.lowpass[i].gain;
				float y = x + cubic.lowpass[i].z1[ch];
				cubic.lowpass[i].z1[ch] = 2.0f * x + cubic.lowpass[i].z2[ch] - cubic.lowpass[i].a1 * y;
				cubic.lowpass[i].z2[ch] = x - cubic.lowpass[i].a2 * y;
				stereo[ch] = y;
			}
		}
		output[n].left = (int16_t)lrintf(fminf(32767, fmaxf(-32768, stereo[0])));
		output[n].right = (int16_t)lrintf(fminf(32767, fmaxf(-32768, stereo[1])));
	}
}

static inline int16_t cubic_sample(int16_t y0, int16_t y1, int16_t y2, int16_t y3, float mu)
{
	float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
	float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
	float c = -0.5f * y0 + 0.5f * y2;
	float out = ((a * mu + b) * mu + c) * mu + y1;
	if (out > 32767.0f)
		return 32767;
	if (out < -32768.0f)
		return -32768;
	return (int16_t)lrintf(out);
}

ResampledFrames cubic_resampler_process(const SND_Frame *input_frames, int input_frame_count,
										int input_sample_rate, int output_sample_rate, double final_ratio)
{
	if (cubic.rate_in != input_sample_rate || cubic.rate_out != output_sample_rate)
	{
		cubic_resampler_reset();
		cubic.rate_in = input_sample_rate;
		cubic.rate_out = output_sample_rate;
	}

	SND_Frame filtered_stack[CUBIC_STACK_FRAMES];
	SND_Frame *filtered_heap = NULL;
	if (input_sample_rate > output_sample_rate || final_ratio < 0.95 || cubic.filtered_ratio) {
		SND_Frame *filtered = filtered_stack;
		if (input_frame_count > CUBIC_STACK_FRAMES) {
			filtered_heap = (SND_Frame *)malloc(input_frame_count * sizeof(SND_Frame));
			if (!filtered_heap) {
				fprintf(stderr, "Error allocating filter buffer\n");
				exit(1);
			}
			filtered = filtered_heap;
		}
		cubic_filter(input_frames, filtered, input_frame_count, final_ratio);
		input_frames = filtered;
	}

	// Index i < 3 is carried history, index i >= 3 is input_frames[i - 3].
	uint64_t step = (uint64_t)((double)(1ULL << 32) / final_ratio + 0.5);
	uint64_t end = (uint64_t)(input_frame_count + 1) << 32;
	int output_frame_count = cubic.pos < end ? (int)((end - cubic.pos + step - 1) / step) : 0;

	SND_Frame *output_frames = (SND_Frame *)malloc((output_frame_count ? output_frame_count : 1) * sizeof(SND_Frame));
	if (!output_frames)
	{
		free(filtered_heap);
		fprintf(stderr, "Error allocating output frames\n");
		exit(1);
	}

	for (int i = 0; i < output_frame_count; i++)
	{
		int idx = (int)(cubic.pos >> 32);
		float mu = (float)(uint32_t)cubic.pos * (1.0f / 4294967296.0f);
		const SND_Frame *f[4];
		for (int k = 0; k < 4; k++)
		{
			int j = idx - 1 + k;
			f[k] = j < 3 ? &cubic.history[j] : &input_frames[j - 3];
		}
		output_frames[i].left = cubic_sample(f[0]->left, f[1]->left, f[2]->left, f[3]->left, mu);
		output_frames[i].right = cubic_sample(f[0]->right, f[1]->right, f[2]->right, f[3]->right, mu);
		cubic.pos += step;
	}

	cubic.pos -= (uint64_t)input_frame_count << 32;
	SND_Frame history[3];
	for (int k = 0; k < 3; k++)
	{
		int j = input_frame_count + k;
		history[k] = j < 3 ? cubic.history[j] : input_frames[j - 3];
	}
	memcpy(cubic.history, history, sizeof(history));
	free(filtered_heap);

	ResampledFrames resampled;
	resampled.frames = output_frames;
	resampled.frame_count = output_frame_count;
	resampled.input_used = input_frame_count;
	return resampled;
}

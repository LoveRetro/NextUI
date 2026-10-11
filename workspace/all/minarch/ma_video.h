#pragma once

#include <stdint.h>
#include <stdlib.h>

void drawRect(int x, int y, int w, int h, uint32_t c, uint32_t *data, int stride);
void fillRect(int x, int y, int w, int h, uint32_t c, uint32_t *data, int stride);
void drawGauge(int x, int y, float percent, int width, int height, uint32_t *data, int stride);
void applyFadeIn(uint32_t **data, size_t pitch, unsigned width, unsigned height, int *frame_counter, int max_frames);
void selectScaler(int src_w, int src_h, int src_p);
void video_refresh_callback(const void* data, unsigned width, unsigned height, size_t pitch);
void Video_cleanup(void);

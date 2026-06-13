#include <string.h>
#include <math.h>

#include "ma_internal.h"
#include "scaler.h"
#include "ma_video.h"

// When set, video_refresh_callback drops the frame. minarch_forceCoreOptionUpdate()
// uses this to run one core frame purely to trigger check_variables() without flashing.
int skip_video_output = 0;


static const char* bitmap_font[] = {
	['0'] = 
		" 111 "
		"1   1"
		"1   1"
		"1  11"
		"1 1 1"
		"11  1"
		"1   1"
		"1   1"
		" 111 ",
	['1'] =
		"   1 "
		" 111 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 ",
	['2'] =
		" 111 "
		"1   1"
		"    1"
		"   1 "
		"  1  "
		" 1   "
		"1    "
		"1    "
		"11111",
	['3'] =
		" 111 "
		"1   1"
		"    1"
		"    1"
		" 111 "
		"    1"
		"    1"
		"1   1"
		" 111 ",
	['4'] =
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"11111"
		"    1"
		"    1",
	['5'] =
		"11111"
		"1    "
		"1    "
		"1111 "
		"    1"
		"    1"
		"    1"
		"1   1"
		" 111 ",
	['6'] =
		" 111 "
		"1    "
		"1    "
		"1111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		" 111 ",
	['7'] =
		"11111"
		"    1"
		"    1"
		"   1 "
		"  1  "
		"  1  "
		"  1  "
		"  1  "
		"  1  ",
	['8'] =
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		" 111 ",
	['9'] =
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		" 1111"
		"    1"
		"    1"
		" 111 ",
	['.'] = 
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		" 11  "
		" 11  ",
	[','] = 
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"  1  "
		"  1  "
		" 1   ",
	[' '] = 
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     ",
	['('] = 
		"   1 "
		"  1  "
		" 1   "
		" 1   "
		" 1   "
		" 1   "
		" 1   "
		"  1  "
		"   1 ",
	[')'] = 
		" 1   "
		"  1  "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"  1  "
		" 1   ",
	['/'] = 
		"   1 "
		"   1 "
		"   1 "
		"  1  "
		"  1  "
		"  1  "
		" 1   "
		" 1   "
		" 1   ",
	['x'] = 
		"     "
		"     "
		"1   1"
		"1   1"
		" 1 1 "
		"  1  "
		" 1 1 "
		"1   1"
		"1   1",
	['%'] = 
		" 1   "
		"1 1  "
		"1 1 1"
		" 1 1 "
		"  1  "
		" 1 1 "
		"1 1 1"
		"  1 1"
		"   1 ",
	['-'] =
		"     "
		"     "
		"     "
		"     "
		" 111 "
		"     "
		"     "
		"     "
		"     ",
	['c'] = 
        "     "
        "     "
        " 111 "
        "1   1"
        "1    "
        "1    "
        "1    "
        "1   1"
        " 111 ",
	['m'] = 
        "     "
        "     "
        "11 11"
        "1 1 1"
        "1 1 1"
        "1   1"
        "1   1"
        "1   1"
        "1   1",
	['z'] =
		"     "
        "     "
        "     "
        "11111"
        "   1 "
        "  1  "
        " 1   "
        "1    "
        "11111",
	['h'] =
		"     "
        "1    "
        "1    "
        "1    "
        "1111 "
        "1   1"
        "1   1"
        "1   1"
        "1   1",
	['D'] = 
		"1111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1111 ",
	['J'] = 
		"  111"
		"    1"
		"    1"
		"    1"
		"    1"
		"1   1"
		"1   1"
		"1   1"
		" 111 ",
	['A'] = 
		"  1  "
		" 1 1 "
		"1   1"
		"1   1"
		"11111"
		"1   1"
		"1   1"
		"1   1"
		"1   1",
	['M'] = 
		"1   1"
		"11 11"
		"1 1 1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1",
	[':'] = 
		"     "
		"     "
		"  1  "
		"     "
		"     "
		"     "
		"  1  "
		"     "
		"     ",
	['B'] = 
		"1111 "
		"1   1"
		"1   1"
		"1111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1111 ",
	['C'] = 
		" 111 "
		"1   1"
		"1    "
		"1    "
		"1    "
		"1    "
		"1    "
		"1   1"
		" 111 ",
	['N'] = 
		"1   1"
		"1   1"
		"11  1"
		"1   1"
		"1 1 1"
		"1   1"
		"1  11"
		"1   1"
		"1   1",
	['H'] = 
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"11111"
		"1   1"
		"1   1"
		"1   1"
		"1   1",
};

void drawRect(int x, int y, int w, int h, uint32_t c, uint32_t *data, int stride) {
	for (int _x = x; _x < x + w; _x++) {
		data[_x + y * stride] = c;
		data[_x + (y + h - 1) * stride] = c;
	}
	for (int _y = y; _y < y + h; _y++) {
		data[x + _y * stride] = c;
		data[x + w - 1 + _y * stride] = c;
	}
}

void fillRect(int x, int y, int w, int h, uint32_t c, uint32_t *data, int stride) {
	for (int _y = y; _y < y + h; _y++) {
		for (int _x = x; _x < x + w; _x++) {
			data[_x + _y * stride] = c;
		}
	}
}

static void blitBitmapText(char* text, int ox, int oy, uint32_t* data, int stride, int width, int height) {
	#define DEBUG_CHAR_WIDTH 5
	#define DEBUG_CHAR_HEIGHT 9
	#define LETTERSPACING 1

	int len = strlen(text);
	int w = ((DEBUG_CHAR_WIDTH + LETTERSPACING) * len) - 1;
	int h = DEBUG_CHAR_HEIGHT;

	if (ox < 0) ox = width - w + ox;
	if (oy < 0) oy = height - h + oy;

	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;

	// Clamp to screen bounds (optional but recommended)
	if (ox + w > width) w = width - ox;
	if (oy + h > height) h = height - oy;

	if (w <= 0 || h <= 0) return;

	// Draw background rectangle (black ARGB8888)
	fillRect(ox, oy, w, h, 0xFF000000, data, stride);

	data += oy * stride + ox;

	for (int y = 0; y < h; y++) {
		// uint32_t* row = data + y * stride;
		int current_x = 0;
		for (int i = 0; i < len; i++) {
			const char* c = bitmap_font[(unsigned char)text[i]];
			if (!c) c = bitmap_font[' '];
			for (int x = 0; x < DEBUG_CHAR_WIDTH; x++) {
				if (current_x >= w) break;

				if (c[y * DEBUG_CHAR_WIDTH + x] == '1') {
					data[y * stride + current_x] = 0xFFFFFFFF;  // white ARGBB8888
				}
				current_x++;
			}
			if (current_x >= w) break;
			current_x += LETTERSPACING;
		}
	}
}

void drawGauge(int x, int y, float percent, int width, int height, uint32_t *data, int stride) {
	// Clamp percent to 0.0 - 1.0
	if (percent < 0.0f) percent = 0.0f;
	if (percent > 1.0f) percent = 1.0f;

	uint8_t red   = (uint8_t)(percent * 255.0f);
	uint8_t green = (uint8_t)((1.0f - percent) * 255.0f);
	uint8_t blue  = 0;
	uint8_t alpha = 255;

	uint32_t fillColor = (red << 24) | (green << 16) | (blue << 8) | alpha;
	uint32_t borderColor = 0xFFFFFFFF;  // White ARGB
	uint32_t bgColor = 0xFF000000;      // Black ARGB

	// Background
	fillRect(x, y, width, height, bgColor, data, stride);

	// Filled portion
	int filledWidth = (int)(percent * width);
	fillRect(x, y, filledWidth, height, fillColor, data, stride);

	// Outline
	drawRect(x, y, width, height, borderColor, data, stride);
}

///////////////////////////////

void selectScaler(int src_w, int src_h, int src_p) {
	int src_x,src_y,dst_x,dst_y,dst_w,dst_h,dst_p,scale;
	double aspect;
	
	int aspect_w = src_w;
	int aspect_h = CEIL_DIV(aspect_w, core.aspect_ratio);
	
	if (aspect_h<src_h) {
		aspect_h = src_h;
		aspect_w = aspect_h * core.aspect_ratio;
		aspect_w += aspect_w % 2;
	}

	char scaler_name[16];
	
	src_x = 0;
	src_y = 0;
	dst_x = 0;
	dst_y = 0;

	// unmodified by crop
	renderer.true_w = src_w;
	renderer.true_h = src_h;
	
	// TODO: this is saving non-rgb30 devices from themselves...or rather, me
	int scaling = screen_scaling;
	if (scaling==SCALE_CROPPED && DEVICE_WIDTH==HDMI_WIDTH) {
		scaling = SCALE_NATIVE;
	}
	
	if (scaling==SCALE_NATIVE || scaling==SCALE_CROPPED) {
		// this is the same whether fit or oversized
		scale = MIN(DEVICE_WIDTH/src_w, DEVICE_HEIGHT/src_h);
		if (!scale) {
			sprintf(scaler_name, "forced crop");
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;
			
			int ox = (DEVICE_WIDTH  - src_w) / 2; // may be negative
			int oy = (DEVICE_HEIGHT - src_h) / 2; // may be negative
			
			if (ox<0) src_x = -ox;
			else dst_x = ox;
			
			if (oy<0) src_y = -oy;
			else dst_y = oy;
		}
		// TODO: this is all kinds of messed up
		// TODO: is this blowing up because the smart has to rotate before scaling?
		// TODO: or is it just that I'm trying to cram 4 logical rects into 2 rect arguments
		// TODO: eg. src.size + src.clip + dst.size + dst.clip
		else if (scaling==SCALE_CROPPED) {
			int scale_x = CEIL_DIV(DEVICE_WIDTH, src_w);
			int scale_y = CEIL_DIV(DEVICE_HEIGHT, src_h);
			scale = MIN(scale_x, scale_y);

			sprintf(scaler_name, "cropped");
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;

			int scaled_w = src_w * scale;
			int scaled_h = src_h * scale;

			int ox = (DEVICE_WIDTH  - scaled_w) / 2; // may be negative
			int oy = (DEVICE_HEIGHT - scaled_h) / 2; // may be negative

			if (ox<0) {
				src_x = -ox / scale;
				src_w -= src_x * 2;
			}
			else {
				dst_x = ox;
				// dst_w -= ox * 2;
			}

			if (oy<0) {
				src_y = -oy / scale;
				src_h -= src_y * 2;
			}
			else {
				dst_y = oy;
				// dst_h -= oy * 2;
			}
		}
		else {
			sprintf(scaler_name, "integer");
			int scaled_w = src_w * scale;
			int scaled_h = src_h * scale;
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;
			dst_x = (DEVICE_WIDTH  - scaled_w) / 2; // should always be positive
			dst_y = (DEVICE_HEIGHT - scaled_h) / 2; // should always be positive
		}
	}
	else {
		int scale_x = CEIL_DIV(DEVICE_WIDTH, src_w);
		int scale_y = CEIL_DIV(DEVICE_HEIGHT,src_h);
		
		// odd resolutions (eg. PS1 Rayman: 320x239) is throwing this off, need to snap to eights
		int r = (DEVICE_HEIGHT-src_h)%8;
		if (r && r<8) scale_y -= 1;
		
		scale = MAX(scale_x, scale_y);
		// if (scale>4) scale = 4;
		// if (scale>2) scale = 4; // TODO: restore, requires sanity checking
		
		int scaled_w = src_w * scale;
		int scaled_h = src_h * scale;
		
		if (scaling==SCALE_FULLSCREEN) {
			sprintf(scaler_name, "full%i", scale);
			// type = 'full (oversized)';
			dst_w = scaled_w;
			dst_h = scaled_h;
			dst_p = dst_w * FIXED_BPP;
		}
		else if (scaling==SCALE_ASPECT_SCREEN) {
	
			int scale_x = DEVICE_WIDTH / src_w;
			int scale_y = DEVICE_HEIGHT / src_h;
			
			// Use the smaller scale to ensure it fits on screen
			scale = MIN(scale_x, scale_y);
			aspect = (double)src_w / src_h;
			
			// Optionally, clamp to a max scale (e.g., 4x) if needed
			// if (scale > 4) scale = 4;
			
			int scaled_w = src_w * scale;
			int scaled_h = src_h * scale;
			
			// Center the image on screen
			dst_w = scaled_w;
			dst_h = scaled_h;
			dst_x = (DEVICE_WIDTH - dst_w) / 2;
			dst_y = (DEVICE_HEIGHT - dst_h) / 2;
			
			dst_p = dst_w * FIXED_BPP;
			
			sprintf(scaler_name, "raw%i", scale);
		}
		else {
			double src_aspect_ratio = ((double)src_w) / src_h;
			// double core_aspect_ratio
			double fixed_aspect_ratio = ((double)DEVICE_WIDTH) / DEVICE_HEIGHT;
			int core_aspect = core.aspect_ratio * 1000;
			int fixed_aspect = fixed_aspect_ratio * 1000;
			
			// still having trouble with FC's 1.306 (13/10? wtf) on 4:3 devices
			// specifically I think it has trouble when src, core, and fixed 
			// ratios don't match
			
			// it handles src and core matching but fixed not, eg. GB and GBA 
			// or core and fixed matching but not src, eg. odd PS resolutions
			
			// we need to transform the src size to core aspect
			// then to fixed aspect
						
			if (core_aspect>fixed_aspect) {
				sprintf(scaler_name, "aspect%iL", scale);
				// letterbox
				// dst_w = scaled_w;
				// dst_h = scaled_w / fixed_aspect_ratio;
				// dst_h += dst_h%2;
				int aspect_h = DEVICE_WIDTH / core.aspect_ratio;
				double aspect_hr = ((double)aspect_h) / DEVICE_HEIGHT;
				dst_w = scaled_w;
				dst_h = scaled_h / aspect_hr;

				dst_y = (dst_h - scaled_h) / 2;
			}
			else if (core_aspect<fixed_aspect) {
				sprintf(scaler_name, "aspect%iP", scale);
				// pillarbox
				// dst_w = scaled_h * fixed_aspect_ratio;
				// dst_w += dst_w%2;
				// dst_h = scaled_h;
				aspect_w = DEVICE_HEIGHT * core.aspect_ratio;
				double aspect_wr = ((double)aspect_w) / DEVICE_WIDTH;
				dst_w = scaled_w / aspect_wr;
				dst_h = scaled_h;
				
				dst_w = (dst_w/8)*8;
				dst_x = (dst_w - scaled_w) / 2;
			}
			else {
				sprintf(scaler_name, "aspect%iM", scale);
				// perfect match
				dst_w = scaled_w;
				dst_h = scaled_h;
			}
			dst_p = dst_w * FIXED_BPP;
		}
	}
	
	renderer.src_x = src_x;
	renderer.src_y = src_y;
	renderer.src_w = src_w;
	renderer.src_h = src_h;
	renderer.src_p = src_p;

	renderer.dst_x = dst_x;
	renderer.dst_y = dst_y;
	renderer.dst_w = dst_w;
	renderer.dst_h = dst_h;
	renderer.dst_p = dst_p;

	renderer.scale = scale;
	renderer.aspect = (scaling==SCALE_ASPECT_SCREEN) ? aspect: (scaling==SCALE_NATIVE||scaling==SCALE_CROPPED)?0:(scaling==SCALE_FULLSCREEN?-1:core.aspect_ratio);
	renderer.blit = GFX_getScaler(&renderer);
}
static void screen_flip(SDL_Surface* screen) {
	
	if (use_core_fps) {
		GFX_flip_fixed_rate(screen, core.fps);
	}
	else {
		GFX_GL_Swap();
		// GFX_flip(screen);
	}
}

// couple of animation functions for pixel data keeping them all cause wanna use them later
void applyFadeIn(uint32_t **data, size_t pitch, unsigned width, unsigned height, int *frame_counter, int max_frames) {
    size_t pixels_per_row = pitch / sizeof(uint32_t);
    static uint32_t temp_buffer[1920 * 1080];

    if (*frame_counter >= max_frames) {
        return;
    }

    float progress = (float)(*frame_counter) / (float)max_frames;
    float eased = progress * progress * (3 - 2 * progress);

    float fade_alpha = eased;

    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            size_t idx = y * pixels_per_row + x;

            uint32_t color = (*data)[idx];

			uint8_t a = (color >> 24) & 0xFF;
			uint8_t b = (color >> 16) & 0xFF;
			uint8_t g = (color >> 8) & 0xFF;
			uint8_t r = (color >> 0) & 0xFF;

            r = (uint8_t)(r * fade_alpha);
            g = (uint8_t)(g * fade_alpha);
            b = (uint8_t)(b * fade_alpha);
            a = (uint8_t)(a * fade_alpha);

			temp_buffer[idx] = ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
        }
    }

    (*frame_counter)++;
    *data = temp_buffer;
}

static void drawDebugHud(const void* data, unsigned width, unsigned height, size_t pitch, enum retro_pixel_format fmt)
{
	if (show_debug && !isnan(perf.ratio) && !isnan(perf.fps) && !isnan(perf.req_fps)  && !isnan(perf.buffer_ms) &&
		perf.buffer_size >= 0  && perf.buffer_free >= 0 && SDL_GetTicks() > 5000) {
		int x = 2 + renderer.src_x;
		int y = 2 + renderer.src_y;
		char debug_text[250];
		int scale = renderer.scale;
		if (scale==-1) scale = 1; // nearest neighbor flag

		sprintf(debug_text, "%ix%i %ix %i/%i", renderer.src_w,renderer.src_h, scale,perf.samplerate_in,perf.samplerate_out);
		blitBitmapText(debug_text,x,y,(uint32_t*)data,pitch / 4, width,height);
		
		sprintf(debug_text, "%.03f/%i/%.0f/%i/%i/%i", perf.ratio,
				perf.buffer_size,perf.buffer_ms, perf.buffer_free, perf.buffer_target,perf.avg_buffer_free);
		blitBitmapText(debug_text, x, y + 14, (uint32_t*)data, pitch / 4, width,
					height);

		sprintf(debug_text, "%i,%i %ix%i", renderer.dst_x,renderer.dst_y, renderer.src_w*scale,renderer.src_h*scale);
		blitBitmapText(debug_text,-x,y,(uint32_t*)data,pitch / 4, width,height);
	
		sprintf(debug_text, "%ix%i,%i", renderer.dst_w,renderer.dst_h, fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 8888 : 565);
		blitBitmapText(debug_text,-x,-y,(uint32_t*)data,pitch / 4, width,height);

		// Frame timing stats
		sprintf(debug_text, "%.1f/%.1f A:%.1f M:%.1f D:%d", perf.fps, perf.req_fps, perf.avg_frame_ms, perf.max_frame_ms, perf.frame_drops);
		blitBitmapText(debug_text,x,-y,(uint32_t*)data,pitch / 4, width,height);
		
		// CPU stats
		PLAT_getCPUSpeed();
		PLAT_getCPUTemp();
		sprintf(debug_text, "%.0f%%/%ihz/%ic", perf.cpu_usage, perf.cpu_speed, perf.cpu_temp);
		blitBitmapText(debug_text,x,-y - 14,(uint32_t*)data,pitch / 4, width,height);
		
		// GPU stats
		PLAT_getGPUUsage();
		PLAT_getGPUSpeed();
		PLAT_getGPUTemp();
		sprintf(debug_text, "%.0f%%/%ihz/%ic", perf.gpu_usage, perf.gpu_speed, perf.gpu_temp);
		blitBitmapText(debug_text,x,-y - 28,(uint32_t*)data,pitch / 4, width,height);

		if(currentshaderpass>0) {
			sprintf(debug_text, "%i/%ix%i/%ix%i/%ix%i", currentshaderpass, currentshadersrcw,currentshadersrch,currentshadertexw,currentshadertexh,currentshaderdstw,currentshaderdsth);
			blitBitmapText(debug_text,x,-y - 42,(uint32_t*)data,pitch / 4, width,height);
		}
	
		double buffer_fill = (double) (perf.buffer_size - perf.buffer_free) / (double) perf.buffer_size;
		drawGauge(x, y + 30, buffer_fill, width / 2, 8, (uint32_t*)data, pitch / 4);
	}
}

static void video_refresh_callback_main(const void *data, unsigned width, unsigned height, size_t pitch) {
	// return;

	Special_render();
	
	static uint32_t last_flip_time = 0;
	
	// 10 seems to be the sweet spot that allows 2x in NES and SNES and 8x in GB at 60fps
	// 14 will let GB hit 10x but NES and SNES will drop to 1.5x at 30fps (not sure why)
	// but 10 hurts PS...
	// TODO: 10 was based on rg35xx, probably different results on other supported platforms
	if (fast_forward && SDL_GetTicks()-last_flip_time<10) return;
	
	// FFVII menus 
	// 16: 30/200
	// 15: 30/180
	// 14: 45/180
	// 12: 30/150
	// 10: 30/120 (optimize text off has no effect)
	//  8: 60/210 (with optimize text off)
	// eg. PS@10 60/240
	if (!data) {
		return;
	}

	// if source has changed size (or forced by dst_p==0)
	// eg. true src + cropped src + fixed dst + cropped dst
	if (renderer.dst_p==0 || width!=renderer.true_w || height!=renderer.true_h) {
		selectScaler(width, height, pitch);
		GFX_clearAll();
		if (!shader_reset_suppressed) {
			GFX_resetShaders();
		} else {
			shader_reset_suppressed = 0; // consume suppression after one use
		}
	}
	
	// debug
	drawDebugHud(data, width, height, pitch, fmt);
	
	static int frame_counter = 0;
	const int max_frames = 8; 
	if(frame_counter<9) {
		applyFadeIn((uint32_t **) &data, pitch, width, height, &frame_counter, max_frames);
	}

	renderer.src = (void*)data;
	renderer.dst = screen->pixels;
	GFX_blitRenderer(&renderer);

	screen_flip(screen);
	last_flip_time = SDL_GetTicks();
}

const void* lastframe = NULL;
static Uint32* rgbaData = NULL;
static size_t rgbaDataSize = 0;

// ARM NEON SIMD optimization for pixel format conversion
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>

// Convert 8 RGB565 pixels to RGBA using NEON (processes 16 bytes → 32 bytes)
static inline void convert_rgb565_to_rgba_neon(const uint16_t* __restrict src, uint32_t* __restrict dst) {
	// Load 8 RGB565 pixels (128 bits)
	uint16x8_t rgb565 = vld1q_u16(src);
	
	// Extract RGB components using bit manipulation
	// R: bits 11-15 (5 bits) → scale to 8 bits
	// G: bits 5-10 (6 bits) → scale to 8 bits  
	// B: bits 0-4 (5 bits) → scale to 8 bits
	
	uint8x8_t r5 = vmovn_u16(vshrq_n_u16(vandq_u16(rgb565, vdupq_n_u16(0xF800)), 11));
	uint8x8_t g6 = vmovn_u16(vshrq_n_u16(vandq_u16(rgb565, vdupq_n_u16(0x07E0)), 5));
	uint8x8_t b5 = vmovn_u16(vandq_u16(rgb565, vdupq_n_u16(0x001F)));
	
	// Scale 5-bit to 8-bit: (val * 255) / 31 ≈ (val << 3) | (val >> 2)
	// Scale 6-bit to 8-bit: (val * 255) / 63 ≈ (val << 2) | (val >> 4)
	uint8x8_t r8 = vorr_u8(vshl_n_u8(r5, 3), vshr_n_u8(r5, 2));
	uint8x8_t g8 = vorr_u8(vshl_n_u8(g6, 2), vshr_n_u8(g6, 4));
	uint8x8_t b8 = vorr_u8(vshl_n_u8(b5, 3), vshr_n_u8(b5, 2));
	uint8x8_t a8 = vdup_n_u8(0xFF);
	
	// Interleave RGBA
	uint8x8x4_t rgba;
	rgba.val[0] = r8;
	rgba.val[1] = g8;
	rgba.val[2] = b8;
	rgba.val[3] = a8;
	
	// Store as RGBA (32 bytes)
	vst4_u8((uint8_t*)dst, rgba);
}

// Convert 4 XRGB8888 pixels to RGBA using NEON (processes 16 bytes → 16 bytes)
static inline void convert_xrgb8888_to_rgba_neon(const uint32_t* __restrict src, uint32_t* __restrict dst) {
	// Load 4 XRGB8888 pixels
	uint32x4_t xrgb = vld1q_u32(src);
	
	// XRGB8888: 0xXXRRGGBB → RGBA: 0xAABBGGRR
	// Extract components
	uint32x4_t r = vandq_u32(vshrq_n_u32(xrgb, 16), vdupq_n_u32(0xFF));
	uint32x4_t g = vandq_u32(vshrq_n_u32(xrgb, 8), vdupq_n_u32(0xFF));
	uint32x4_t b = vandq_u32(xrgb, vdupq_n_u32(0xFF));
	uint32x4_t a = vdupq_n_u32(0xFF);
	
	// Reconstruct as RGBA
	uint32x4_t rgba = vorrq_u32(vorrq_u32(r, vshlq_n_u32(g, 8)), vorrq_u32(vshlq_n_u32(b, 16), vshlq_n_u32(a, 24)));
	
	vst1q_u32(dst, rgba);
}
#endif

// Convert XRGB8888 to RGBA format (handles pitch correctly)
static void convert_xrgb8888_to_rgba(const void* src, uint32_t* dst, unsigned width, unsigned height, size_t pitch) {
	const uint32_t* srcData = (const uint32_t*)src;
	unsigned srcPitchInPixels = pitch / sizeof(uint32_t);
	
	for (unsigned y = 0; y < height; y++) {
		const uint32_t* srcRow = srcData + y * srcPitchInPixels;
		uint32_t* dstRow = dst + y * width;
		unsigned x = 0;

#if defined(__ARM_NEON) || defined(__aarch64__)
		// NEON: process 4 pixels at a time
		for (; x + 3 < width; x += 4) {
			convert_xrgb8888_to_rgba_neon(srcRow + x, dstRow + x);
		}
#else
		// Scalar: process 4 pixels at a time for better cache utilization
		for (; x + 3 < width; x += 4) {
			uint32_t p0 = srcRow[x], p1 = srcRow[x+1], p2 = srcRow[x+2], p3 = srcRow[x+3];
			
			// Swizzle: XRGB -> RGBA (swap R and B, set A=0xFF)
			dstRow[x]   = (p0 & 0x0000FF00) | ((p0 & 0x00FF0000) >> 16) | ((p0 & 0x000000FF) << 16) | 0xFF000000;
			dstRow[x+1] = (p1 & 0x0000FF00) | ((p1 & 0x00FF0000) >> 16) | ((p1 & 0x000000FF) << 16) | 0xFF000000;
			dstRow[x+2] = (p2 & 0x0000FF00) | ((p2 & 0x00FF0000) >> 16) | ((p2 & 0x000000FF) << 16) | 0xFF000000;
			dstRow[x+3] = (p3 & 0x0000FF00) | ((p3 & 0x00FF0000) >> 16) | ((p3 & 0x000000FF) << 16) | 0xFF000000;
		}
#endif
		// Handle remaining pixels in the row
		for (; x < width; x++) {
			uint32_t pixel = srcRow[x];
			dstRow[x] = (pixel & 0x0000FF00) | ((pixel & 0x00FF0000) >> 16) | ((pixel & 0x000000FF) << 16) | 0xFF000000;
		}
	}
}

// Convert RGB565 to RGBA format (handles pitch correctly)
static void convert_rgb565_to_rgba(const void* src, uint32_t* dst, unsigned width, unsigned height, size_t pitch) {
	const uint16_t* srcData = (const uint16_t*)src;
	unsigned srcPitchInPixels = pitch / sizeof(uint16_t);
	
	for (unsigned y = 0; y < height; y++) {
		const uint16_t* srcRow = srcData + y * srcPitchInPixels;
		uint32_t* dstRow = dst + y * width;
		unsigned x = 0;

#if defined(__ARM_NEON) || defined(__aarch64__)
		// NEON: process 8 pixels at a time
		for (; x + 7 < width; x += 8) {
			convert_rgb565_to_rgba_neon(srcRow + x, dstRow + x);
		}
#else
		// Scalar: process 4 pixels at a time
		for (; x + 3 < width; x += 4) {
			uint16_t p0 = srcRow[x], p1 = srcRow[x+1], p2 = srcRow[x+2], p3 = srcRow[x+3];
			
			uint8_t r0 = (p0 >> 11) & 0x1F, g0 = (p0 >> 5) & 0x3F, b0 = p0 & 0x1F;
			uint8_t r1 = (p1 >> 11) & 0x1F, g1 = (p1 >> 5) & 0x3F, b1 = p1 & 0x1F;
			uint8_t r2 = (p2 >> 11) & 0x1F, g2 = (p2 >> 5) & 0x3F, b2 = p2 & 0x1F;
			uint8_t r3 = (p3 >> 11) & 0x1F, g3 = (p3 >> 5) & 0x3F, b3 = p3 & 0x1F;
			
			r0 = (r0 << 3) | (r0 >> 2); g0 = (g0 << 2) | (g0 >> 4); b0 = (b0 << 3) | (b0 >> 2);
			r1 = (r1 << 3) | (r1 >> 2); g1 = (g1 << 2) | (g1 >> 4); b1 = (b1 << 3) | (b1 >> 2);
			r2 = (r2 << 3) | (r2 >> 2); g2 = (g2 << 2) | (g2 >> 4); b2 = (b2 << 3) | (b2 >> 2);
			r3 = (r3 << 3) | (r3 >> 2); g3 = (g3 << 2) | (g3 >> 4); b3 = (b3 << 3) | (b3 >> 2);
			
			dstRow[x]   = (0xFF << 24) | (b0 << 16) | (g0 << 8) | r0;
			dstRow[x+1] = (0xFF << 24) | (b1 << 16) | (g1 << 8) | r1;
			dstRow[x+2] = (0xFF << 24) | (b2 << 16) | (g2 << 8) | r2;
			dstRow[x+3] = (0xFF << 24) | (b3 << 16) | (g3 << 8) | r3;
		}
#endif
		// Handle remaining pixels in the row
		for (; x < width; x++) {
			uint16_t pixel = srcRow[x];
			uint8_t r = (pixel >> 11) & 0x1F;
			uint8_t g = (pixel >> 5) & 0x3F;
			uint8_t b = pixel & 0x1F;
			
			r = (r << 3) | (r >> 2);
			g = (g << 2) | (g >> 4);
			b = (b << 3) | (b >> 2);
			
			dstRow[x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
		}
	}
}

void video_refresh_callback(const void* data, unsigned width, unsigned height, size_t pitch) {
	// Log NEON availability once on first call
	static int neon_logged = 0;
	if (!neon_logged) {
		neon_logged = 1;
#if defined(__ARM_NEON) || defined(__aarch64__)
		LOG_info("Pixel conversion: ARM NEON SIMD optimizations enabled\n");
#else
		LOG_info("Pixel conversion: Using scalar optimizations (NEON not available)\n");
#endif
	}

	// Early exit if quitting to avoid rendering stale frames
	if (quit) return;

	// Suppress output for forced option-update frames (minarch_forceCoreOptionUpdate)
	if (skip_video_output) return;

	// Allocate RGBA buffer if needed
	if (!rgbaData || rgbaDataSize != width * height) {
		if (rgbaData) free(rgbaData);
		rgbaDataSize = width * height;
		rgbaData = (Uint32*)malloc(rgbaDataSize * sizeof(Uint32));
		if (!rgbaData) {
			printf("Failed to allocate memory for RGBA data.\n");
			return;
		}
	}

	// Handle NULL data by reusing last frame
	if (!data) {
		data = lastframe;
		if (!data) return;
	} else {
		// Convert pixel format to RGBA
		if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
			convert_xrgb8888_to_rgba(data, rgbaData, width, height, pitch);
		} else {
			convert_rgb565_to_rgba(data, rgbaData, width, height, pitch);
		}
		
		data = rgbaData;
		lastframe = data;
	}
	pitch = width * sizeof(Uint32);

	// Set ambient lighting color (if enabled)
	if (ambient_mode && !fast_forward && data) {
		GFX_setAmbientColor(data, width, height, pitch, ambient_mode);
	}


	// Render the frame
	video_refresh_callback_main(data, width, height, pitch);
}

void Video_cleanup(void) {
	if (rgbaData) {
		free(rgbaData);
		rgbaData = NULL;
	}
}

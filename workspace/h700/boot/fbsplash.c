// Minimal framebuffer splash for the H700 boot shim.
#include <ctype.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
	uint8_t rows[7];
} Glyph;

typedef struct {
	char c;
	Glyph g;
} GlyphMap;

static const GlyphMap glyphs[] = {
	{' ', {{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}},
	{'!', {{0x04,0x04,0x04,0x04,0x04,0x00,0x04}}},
	{'-', {{0x00,0x00,0x00,0x1f,0x00,0x00,0x00}}},
	{'.', {{0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}}},
	{'0', {{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}}},
	{'1', {{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}}},
	{'2', {{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}}},
	{'3', {{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e}}},
	{'4', {{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}}},
	{'5', {{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e}}},
	{'6', {{0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}}},
	{'7', {{0x1f,0x01,0x02,0x04,0x08,0x08,0x08}}},
	{'8', {{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}}},
	{'9', {{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e}}},
	{'A', {{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}}},
	{'B', {{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}}},
	{'C', {{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}}},
	{'D', {{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}}},
	{'E', {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}}},
	{'F', {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}}},
	{'G', {{0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}}},
	{'H', {{0x11,0x11,0x11,0x1f,0x11,0x11,0x11}}},
	{'I', {{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}}},
	{'J', {{0x07,0x02,0x02,0x02,0x12,0x12,0x0c}}},
	{'K', {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}},
	{'L', {{0x10,0x10,0x10,0x10,0x10,0x10,0x1f}}},
	{'M', {{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}}},
	{'N', {{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}},
	{'O', {{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}}},
	{'P', {{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}}},
	{'Q', {{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}}},
	{'R', {{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}}},
	{'S', {{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}}},
	{'T', {{0x1f,0x04,0x04,0x04,0x04,0x04,0x04}}},
	{'U', {{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}}},
	{'V', {{0x11,0x11,0x11,0x11,0x11,0x0a,0x04}}},
	{'W', {{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}}},
	{'X', {{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}}},
	{'Y', {{0x11,0x11,0x0a,0x04,0x04,0x04,0x04}}},
	{'Z', {{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}}},
};

static const Glyph *glyph_for(char c) {
	c = (char)toupper((unsigned char)c);
	for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
		if (glyphs[i].c == c)
			return &glyphs[i].g;
	}
	return &glyphs[0].g;
}

static uint32_t make_pixel(struct fb_var_screeninfo *v, uint8_t r, uint8_t g, uint8_t b) {
	uint32_t pixel = 0;
	pixel |= ((uint32_t)(r >> (8 - v->red.length))) << v->red.offset;
	pixel |= ((uint32_t)(g >> (8 - v->green.length))) << v->green.offset;
	pixel |= ((uint32_t)(b >> (8 - v->blue.length))) << v->blue.offset;
	return pixel;
}

static void put_pixel(uint8_t *fb, struct fb_var_screeninfo *v, struct fb_fix_screeninfo *f, int x, int y, uint32_t pixel) {
	if (x < 0 || y < 0 || x >= (int)v->xres || y >= (int)v->yres)
		return;

	uint8_t *dst = fb + y * f->line_length + x * (v->bits_per_pixel / 8);
	switch (v->bits_per_pixel) {
		case 16:
			*(uint16_t *)dst = (uint16_t)pixel;
			break;
		case 24:
			dst[0] = pixel & 0xff;
			dst[1] = (pixel >> 8) & 0xff;
			dst[2] = (pixel >> 16) & 0xff;
			break;
		case 32:
			*(uint32_t *)dst = pixel;
			break;
	}
}

static void fill_rect(uint8_t *fb, struct fb_var_screeninfo *v, struct fb_fix_screeninfo *f, int x, int y, int w, int h, uint32_t pixel) {
	for (int yy = y; yy < y + h; yy++) {
		for (int xx = x; xx < x + w; xx++)
			put_pixel(fb, v, f, xx, yy, pixel);
	}
}

static int text_width(const char *text, int scale) {
	return (int)strlen(text) * 6 * scale - scale;
}

static void draw_text(uint8_t *fb, struct fb_var_screeninfo *v, struct fb_fix_screeninfo *f, int x, int y, const char *text, int scale, uint32_t pixel) {
	for (const char *p = text; *p; p++, x += 6 * scale) {
		const Glyph *glyph = glyph_for(*p);
		for (int row = 0; row < 7; row++) {
			for (int col = 0; col < 5; col++) {
				if (glyph->rows[row] & (1 << (4 - col)))
					fill_rect(fb, v, f, x + col * scale, y + row * scale, scale, scale, pixel);
			}
		}
	}
}

int main(int argc, char **argv) {
	const char *message = argc > 1 ? argv[1] : "STARTING NEXTUI";
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0)
		return 1;

	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
		close(fd);
		return 1;
	}
	if (v.bits_per_pixel != 16 && v.bits_per_pixel != 24 && v.bits_per_pixel != 32) {
		close(fd);
		return 1;
	}

	size_t map_len = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres;
	uint8_t *fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		close(fd);
		return 1;
	}

	uint32_t bg = make_pixel(&v, 13, 17, 20);
	uint32_t panel = make_pixel(&v, 29, 36, 40);
	uint32_t accent = make_pixel(&v, 85, 214, 183);
	uint32_t text = make_pixel(&v, 235, 241, 241);
	uint32_t dim = make_pixel(&v, 120, 134, 134);

	fill_rect(fb, &v, &f, 0, 0, v.xres, v.yres, bg);
	int scale = v.xres < 600 ? 3 : 4;
	int logo_scale = v.xres < 600 ? 6 : 8;
	int center_x = (int)v.xres / 2;
	int center_y = (int)v.yres / 2;

	int panel_w = (int)v.xres * 7 / 10;
	int panel_h = (int)v.yres / 3;
	int panel_x = center_x - panel_w / 2;
	int panel_y = center_y - panel_h / 2;
	fill_rect(fb, &v, &f, panel_x, panel_y, panel_w, panel_h, panel);
	fill_rect(fb, &v, &f, panel_x, panel_y, panel_w, 4, accent);

	int logo_w = text_width("NEXTUI", logo_scale);
	draw_text(fb, &v, &f, center_x - logo_w / 2, panel_y + panel_h / 4 - 24, "NEXTUI", logo_scale, text);

	char msg[64];
	snprintf(msg, sizeof(msg), "%s", message);
	int msg_w = text_width(msg, scale);
	draw_text(fb, &v, &f, center_x - msg_w / 2, panel_y + panel_h * 2 / 3, msg, scale, dim);

	msync(fb, map_len, MS_SYNC);
	munmap(fb, map_len);
	close(fd);
	return 0;
}

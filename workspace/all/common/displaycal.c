#include "displaycal.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DISP_LCD_SET_GAMMA_TABLE          0x10b
#define DISP_LCD_GAMMA_CORRECTION_ENABLE  0x10c
#define DISP_LCD_GAMMA_CORRECTION_DISABLE 0x10d

#define DISPLAYCAL_LUT_ENTRIES 256
#define DISPLAYCAL_LUT_BYTES   (DISPLAYCAL_LUT_ENTRIES * 4)

typedef struct {
	double red_gain;
	double green_gain;
	double blue_gain;
} DisplayCalGains;

int DisplayCal_clampGainValue(int value) {
	if (value < DISPLAYCAL_GAIN_MIN)
		return DISPLAYCAL_GAIN_MIN;
	if (value > DISPLAYCAL_GAIN_MAX)
		return DISPLAYCAL_GAIN_MAX;
	return value;
}

static unsigned char clamp_u8(double value) {
	if (value < 0.0)
		return 0;
	if (value > 255.0)
		return 255;
	return (unsigned char)(value + 0.5);
}

static double srgb_to_linear(double c) {
	if (c <= 0.04045)
		return c / 12.92;
	return pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c) {
	if (c <= 0.0)
		return 0.0;
	if (c <= 0.0031308)
		return c * 12.92;
	return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static void fill_linear_gain_table(uint32_t table[DISPLAYCAL_LUT_ENTRIES], const DisplayCalGains *gains) {
	for (int i = 0; i < DISPLAYCAL_LUT_ENTRIES; i++) {
		double linear = srgb_to_linear(i / 255.0);
		uint32_t r = clamp_u8(linear_to_srgb(linear * gains->red_gain) * 255.0);
		uint32_t g = clamp_u8(linear_to_srgb(linear * gains->green_gain) * 255.0);
		uint32_t b = clamp_u8(linear_to_srgb(linear * gains->blue_gain) * 255.0);
		table[i] = (r << 16) | (g << 8) | b;
	}
}

static void fill_identity_table(uint32_t table[DISPLAYCAL_LUT_ENTRIES]) {
	for (int i = 0; i < DISPLAYCAL_LUT_ENTRIES; i++)
		table[i] = ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
}

static int open_disp(void) {
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0)
		fprintf(stderr, "displaycal: open /dev/disp failed: %s\n", strerror(errno));
	return fd;
}

static int disable_gamma(int fd, int screen) {
	unsigned long param[4] = { (unsigned long)screen, 0, 0, 0 };
	if (ioctl(fd, DISP_LCD_GAMMA_CORRECTION_DISABLE, param) < 0) {
		fprintf(stderr, "displaycal: disable gamma failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int set_gamma_table(int fd, int screen, uint32_t table[DISPLAYCAL_LUT_ENTRIES]) {
	unsigned long set_param[4] = {
		(unsigned long)screen,
		(unsigned long)table,
		DISPLAYCAL_LUT_BYTES,
		0,
	};

	if (ioctl(fd, DISP_LCD_SET_GAMMA_TABLE, set_param) < 0) {
		fprintf(stderr, "displaycal: set gamma table failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int enable_gamma(int fd, int screen) {
	unsigned long enable_param[4] = { (unsigned long)screen, 0, 0, 0 };
	if (ioctl(fd, DISP_LCD_GAMMA_CORRECTION_ENABLE, enable_param) < 0) {
		fprintf(stderr, "displaycal: enable gamma failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int apply_table(int fd, int screen, uint32_t table[DISPLAYCAL_LUT_ENTRIES]) {
	if (set_gamma_table(fd, screen, table) < 0)
		return -1;
	return enable_gamma(fd, screen);
}

static int reset_table_and_disable(int fd, int screen) {
	uint32_t table[DISPLAYCAL_LUT_ENTRIES];
	fill_identity_table(table);
	if (set_gamma_table(fd, screen, table) < 0)
		return -1;
	return disable_gamma(fd, screen);
}

static int apply_gains(const DisplayCalGains *gains) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	uint32_t table[DISPLAYCAL_LUT_ENTRIES];
	fill_linear_gain_table(table, gains);
	int ret = apply_table(fd, 0, table);
	close(fd);
	return ret;
}

int DisplayCal_enableWithValues(int red_gain, int green_gain, int blue_gain) {
	DisplayCalGains gains = {
		.red_gain = (double)DisplayCal_clampGainValue(red_gain) / DISPLAYCAL_GAIN_SCALE,
		.green_gain = (double)DisplayCal_clampGainValue(green_gain) / DISPLAYCAL_GAIN_SCALE,
		.blue_gain = (double)DisplayCal_clampGainValue(blue_gain) / DISPLAYCAL_GAIN_SCALE,
	};
	return apply_gains(&gains);
}

int DisplayCal_disable(void) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	int ret = reset_table_and_disable(fd, 0);
	close(fd);
	return ret;
}

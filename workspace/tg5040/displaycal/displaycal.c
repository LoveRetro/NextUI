#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "displaycal.h"

#define DISP_LCD_SET_GAMMA_TABLE          0x10b
#define DISP_LCD_GAMMA_CORRECTION_ENABLE  0x10c
#define DISP_LCD_GAMMA_CORRECTION_DISABLE 0x10d

#define DISPLAYCAL_LUT_ENTRIES 256
#define DISPLAYCAL_LUT_BYTES   (DISPLAYCAL_LUT_ENTRIES * 4)

typedef struct {
	int enabled;
	double red_gain;
	double green_gain;
	double blue_gain;
} DisplayCalConfig;

static void init_config(DisplayCalConfig *config) {
	config->enabled = DISPLAYCAL_DEFAULT_ENABLED;
	config->red_gain = DISPLAYCAL_DEFAULT_RED_GAIN;
	config->green_gain = DISPLAYCAL_DEFAULT_GREEN_GAIN;
	config->blue_gain = DISPLAYCAL_DEFAULT_BLUE_GAIN;
}

static double clamp_gain(double v) {
	if (isnan(v))
		return 1.0;
	if (v < (double)DISPLAYCAL_GAIN_MIN / DISPLAYCAL_GAIN_SCALE)
		return (double)DISPLAYCAL_GAIN_MIN / DISPLAYCAL_GAIN_SCALE;
	if (v > (double)DISPLAYCAL_GAIN_MAX / DISPLAYCAL_GAIN_SCALE)
		return (double)DISPLAYCAL_GAIN_MAX / DISPLAYCAL_GAIN_SCALE;
	return v;
}

static unsigned char clamp_u8(double v) {
	if (v < 0.0)
		return 0;
	if (v > 255.0)
		return 255;
	return (unsigned char)(v + 0.5);
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

static int streq(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}

static void fill_linear_gain_table(uint32_t table[DISPLAYCAL_LUT_ENTRIES], const DisplayCalConfig *config) {
	for (int i = 0; i < DISPLAYCAL_LUT_ENTRIES; i++) {
		double linear = srgb_to_linear(i / 255.0);
		uint32_t r = clamp_u8(linear_to_srgb(linear * config->red_gain) * 255.0);
		uint32_t g = clamp_u8(linear_to_srgb(linear * config->green_gain) * 255.0);
		uint32_t b = clamp_u8(linear_to_srgb(linear * config->blue_gain) * 255.0);
		table[i] = (r << 16) | (g << 8) | b;
	}
}

static void fill_identity_table(uint32_t table[DISPLAYCAL_LUT_ENTRIES]) {
	for (int i = 0; i < DISPLAYCAL_LUT_ENTRIES; i++)
		table[i] = ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
}

static void load_config(DisplayCalConfig *config, const char *path) {
	if (!path || !*path)
		return;

	FILE *file = fopen(path, "r");
	if (!file)
		return;

	char line[160];
	while (fgets(line, sizeof(line), file)) {
		// discard the tail of an overlong line so it is not parsed as its own line
		if (!strchr(line, '\n')) {
			int c;
			while ((c = fgetc(file)) != EOF && c != '\n')
				;
		}

		if (strncmp(line, "enabled=", 8) == 0)
			config->enabled = atoi(line + 8) ? 1 : 0;
		else if (strncmp(line, "red=", 4) == 0)
			config->red_gain = clamp_gain(atof(line + 4));
		else if (strncmp(line, "green=", 6) == 0)
			config->green_gain = clamp_gain(atof(line + 6));
		else if (strncmp(line, "blue=", 5) == 0)
			config->blue_gain = clamp_gain(atof(line + 5));
	}

	fclose(file);
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

static int apply_config(const DisplayCalConfig *config) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	int ret = 0;
	if (!config->enabled) {
		ret = reset_table_and_disable(fd, 0);
	} else {
		uint32_t table[DISPLAYCAL_LUT_ENTRIES];
		fill_linear_gain_table(table, config);
		ret = apply_table(fd, 0, table);
	}

	close(fd);
	return ret;
}

static int apply_identity(int screen) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	uint32_t table[DISPLAYCAL_LUT_ENTRIES];
	fill_identity_table(table);
	int ret = apply_table(fd, screen, table);
	close(fd);
	return ret;
}

static int disable_screen(int screen) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	int ret = reset_table_and_disable(fd, screen);
	close(fd);
	return ret;
}

static void print_status(const DisplayCalConfig *config) {
	printf("enabled=%d\n", config->enabled);
	printf("red=%.2f\n", config->red_gain);
	printf("green=%.2f\n", config->green_gain);
	printf("blue=%.2f\n", config->blue_gain);
}

static void usage(const char *argv0) {
	fprintf(stderr,
		"Usage:\n"
		"  %s apply [config]\n"
		"  %s status [config]\n"
		"  %s disable [screen]\n"
		"  %s identity [screen]\n",
		argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (streq(argv[1], "apply")) {
		DisplayCalConfig config;
		init_config(&config);
		if (argc >= 3)
			load_config(&config, argv[2]);
		return apply_config(&config) < 0 ? 1 : 0;
	}

	if (streq(argv[1], "status")) {
		DisplayCalConfig config;
		init_config(&config);
		if (argc >= 3)
			load_config(&config, argv[2]);
		print_status(&config);
		return 0;
	}

	if (streq(argv[1], "disable")) {
		int screen = argc >= 3 ? atoi(argv[2]) : 0;
		return disable_screen(screen) < 0 ? 1 : 0;
	}

	if (streq(argv[1], "identity")) {
		int screen = argc >= 3 ? atoi(argv[2]) : 0;
		return apply_identity(screen) < 0 ? 1 : 0;
	}

	usage(argv[0]);
	return 2;
}

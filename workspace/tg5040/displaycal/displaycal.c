#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DISP_LCD_SET_GAMMA_TABLE          0x10b
#define DISP_LCD_GAMMA_CORRECTION_ENABLE  0x10c
#define DISP_LCD_GAMMA_CORRECTION_DISABLE 0x10d

#define DISPLAYCAL_LUT_ENTRIES 256
#define DISPLAYCAL_LUT_BYTES   (DISPLAYCAL_LUT_ENTRIES * 4)

#define DEFAULT_RED_GAIN   1.0000000000000000
#define DEFAULT_GREEN_GAIN 0.9233642796405507
#define DEFAULT_BLUE_GAIN  0.5833412353395729

typedef struct {
	int enabled;
	int screen;
	double strength;
	double red_gain;
	double green_gain;
	double blue_gain;
	char format[4];
} DisplayCalConfig;

static void init_config(DisplayCalConfig *config) {
	config->enabled = 1;
	config->screen = 0;
	config->strength = 1.0;
	config->red_gain = DEFAULT_RED_GAIN;
	config->green_gain = DEFAULT_GREEN_GAIN;
	config->blue_gain = DEFAULT_BLUE_GAIN;
	strcpy(config->format, "rgb");
}

static double clamp_unit(double v) {
	if (v < 0.0)
		return 0.0;
	if (v > 1.0)
		return 1.0;
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

static double mix_gain(double gain, double strength) {
	return 1.0 + (gain - 1.0) * clamp_unit(strength);
}

static char *trim_token(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;

	char *end = s + strlen(s);
	while (end > s) {
		char c = end[-1];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			break;
		*--end = '\0';
	}

	return s;
}

static int streq(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}

static int valid_format(const char *format) {
	return streq(format, "rgb") ||
		streq(format, "bgr") ||
		streq(format, "grb") ||
		streq(format, "gbr") ||
		streq(format, "rbg") ||
		streq(format, "brg");
}

static uint32_t pack_rgb(int r, int g, int b, const char *format) {
	uint32_t rr = (uint32_t)r & 0xff;
	uint32_t gg = (uint32_t)g & 0xff;
	uint32_t bb = (uint32_t)b & 0xff;

	if (streq(format, "bgr"))
		return (bb << 16) | (gg << 8) | rr;
	if (streq(format, "grb"))
		return (gg << 16) | (rr << 8) | bb;
	if (streq(format, "gbr"))
		return (gg << 16) | (bb << 8) | rr;
	if (streq(format, "rbg"))
		return (rr << 16) | (bb << 8) | gg;
	if (streq(format, "brg"))
		return (bb << 16) | (rr << 8) | gg;

	return (rr << 16) | (gg << 8) | bb;
}

static void fill_linear_gain_table(uint32_t table[DISPLAYCAL_LUT_ENTRIES], const DisplayCalConfig *config) {
	double red_gain = mix_gain(config->red_gain, config->strength);
	double green_gain = mix_gain(config->green_gain, config->strength);
	double blue_gain = mix_gain(config->blue_gain, config->strength);

	for (int i = 0; i < DISPLAYCAL_LUT_ENTRIES; i++) {
		double linear = srgb_to_linear(i / 255.0);
		int r = clamp_u8(linear_to_srgb(linear * red_gain) * 255.0);
		int g = clamp_u8(linear_to_srgb(linear * green_gain) * 255.0);
		int b = clamp_u8(linear_to_srgb(linear * blue_gain) * 255.0);
		table[i] = pack_rgb(r, g, b, config->format);
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
		char *comment = strchr(line, '#');
		if (comment)
			*comment = '\0';

		char *sep = strchr(line, '=');
		if (!sep)
			continue;

		*sep = '\0';
		char *key = trim_token(line);
		char *value = trim_token(sep + 1);

		if (streq(key, "enabled"))
			config->enabled = atoi(value) ? 1 : 0;
		else if (streq(key, "screen"))
			config->screen = atoi(value);
		else if (streq(key, "strength"))
			config->strength = atof(value);
		else if (streq(key, "red") || streq(key, "r") || streq(key, "red_gain"))
			config->red_gain = atof(value);
		else if (streq(key, "green") || streq(key, "g") || streq(key, "green_gain"))
			config->green_gain = atof(value);
		else if (streq(key, "blue") || streq(key, "b") || streq(key, "blue_gain"))
			config->blue_gain = atof(value);
		else if (streq(key, "format") && valid_format(value))
			snprintf(config->format, sizeof(config->format), "%s", value);
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

static int apply_table(int fd, int screen, uint32_t table[DISPLAYCAL_LUT_ENTRIES]) {
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

	unsigned long enable_param[4] = { (unsigned long)screen, 0, 0, 0 };
	if (ioctl(fd, DISP_LCD_GAMMA_CORRECTION_ENABLE, enable_param) < 0) {
		fprintf(stderr, "displaycal: enable gamma failed: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int apply_config(const DisplayCalConfig *config) {
	int fd = open_disp();
	if (fd < 0)
		return -1;

	int ret = 0;
	if (!config->enabled) {
		ret = disable_gamma(fd, config->screen);
	} else {
		uint32_t table[DISPLAYCAL_LUT_ENTRIES];
		fill_linear_gain_table(table, config);
		ret = apply_table(fd, config->screen, table);
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

	int ret = disable_gamma(fd, screen);
	close(fd);
	return ret;
}

static void print_status(const DisplayCalConfig *config) {
	printf("enabled=%d\n", config->enabled);
	printf("screen=%d\n", config->screen);
	printf("strength=%.6f\n", config->strength);
	printf("red=%.16f\n", config->red_gain);
	printf("green=%.16f\n", config->green_gain);
	printf("blue=%.16f\n", config->blue_gain);
	printf("format=%s\n", config->format);
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

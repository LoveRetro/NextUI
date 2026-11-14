// Minimal subset of the sunxi disp2 UAPI (include/video/sunxi_display2.h from
// the 4.9 sun50iw9 BSP kernel) — just enough to read-modify-write the fb0
// scanout layer via DISP_LAYER_GET_CONFIG/DISP_LAYER_SET_CONFIG. Field order,
// sizes and the ioctl numbers must match the kernel ABI exactly; do not edit
// without checking against the BSP header.
#ifndef SUNXI_DISPLAY2_MIN_H
#define SUNXI_DISPLAY2_MIN_H

#include <stdbool.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

struct disp_rect {
	int x;
	int y;
	unsigned int width;
	unsigned int height;
};

struct disp_rectsz {
	unsigned int width;
	unsigned int height;
};

struct disp_rect64 {
	long long x;
	long long y;
	long long width;
	long long height;
};

struct disp_fb_info {
	unsigned long long addr[3];
	struct disp_rectsz size[3];
	unsigned int align[3];
	int format;      /* enum disp_pixel_format */
	int color_space; /* enum disp_color_space */
	unsigned int trd_right_addr[3];
	bool pre_multiply;
	struct disp_rect64 crop;
	int flags; /* enum disp_buffer_flags */
	int scan;  /* enum disp_scan_flags */
};

struct disp_layer_info {
	int mode; /* enum disp_layer_mode */
	unsigned char zorder;
	unsigned char alpha_mode;
	unsigned char alpha_value;
	struct disp_rect screen_win;
	bool b_trd_out;
	int out_trd_mode; /* enum disp_3d_out_mode */
	union {
		unsigned int color;
		struct disp_fb_info fb;
	};
	unsigned int id;
};

struct disp_layer_config {
	struct disp_layer_info info;
	bool enable;
	unsigned int channel;
	unsigned int layer_id;
};

#endif

// h700
#include <stdio.h>
#include <stdlib.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#include <msettings.h>

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "utils.h"

#include "scaler.h"
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#include <dirent.h>
#include <stdint.h>

int panel_w = 640;
int panel_h = 480;
double panel_fps = 60.0;
int needs_portrait_sdl = 0;
int dev_has_lstick = 0;
int dev_has_rstick = 0;
int dev_has_rgb = 0;
int dev_num_leds = 0;
static int wake_fd = -1;

// RGB LEDs hang off an MCU on UART5, gated by the axp2202 mcu_pwr rail.
// See led.c, which is included at the bottom of this file.
#define H700_LED_TTY "/dev/ttyS5"
#define H700_MCU_PWR "/sys/class/power_supply/axp2202-battery/mcu_pwr"
static void LED_invalidateCache(void);
static void LED_shutdown(void);

#define H700_INPUT_COUNT 12
#define EV_KEY 0x01
#define EV_ABS 0x03

// Built-in controls use raw evdev codes. The in-tree SDL patch restores joystick
// enumeration, but platform input deliberately skips this device because its SDL
// button ordering differs from the external-pad JOY_* mapping and would duplicate
// the evdev events.
#define RAW_HATY 17
#define RAW_HATX 16
#define RAW_LSY  3
#define RAW_LSX  2
#define RAW_RSY  5
#define RAW_RSX  4

struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

static int input_fds[H700_INPUT_COUNT];
static uint32_t last_input_scan = 0;

static void close_evdev_input(int i) {
	if (i < 0 || i >= H700_INPUT_COUNT || input_fds[i] < 0)
		return;
	close(input_fds[i]);
	input_fds[i] = -1;
}

static void open_evdev_input(int i) {
	char path[64];
	snprintf(path, sizeof(path), "/dev/input/event%i", i);

	input_fds[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (input_fds[i] < 0)
		return;

	char name_path[128];
	char name[256] = {0};
	snprintf(name_path, sizeof(name_path), "/sys/class/input/event%i/device/name", i);
	getFile(name_path, name, sizeof(name));
	if (name[0])
		LOG_info("Opening input event%i: %s\n", i, name);
	else
		LOG_info("Opening input event%i\n", i);
}

static void scan_evdev_inputs(void) {
	uint32_t now = SDL_GetTicks();
	if (last_input_scan && now - last_input_scan < 2000)
		return;
	last_input_scan = now;

	for (int i = 0; i < H700_INPUT_COUNT; i++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/input/event%i", i);
		int connected = exists(path);
		if (input_fds[i] < 0 && connected)
			open_evdev_input(i);
		else if (input_fds[i] >= 0 && !connected)
			close_evdev_input(i);
	}
}

static void apply_button_state(int btn, int id, int pressed, uint32_t tick) {
	if (btn == BTN_NONE || id < 0)
		return;

	if (!pressed) {
		if (pad.is_pressed & btn) {
			pad.is_pressed &= ~btn;
			pad.just_repeated &= ~btn;
			pad.just_released |= btn;
		}
	}
	else if ((pad.is_pressed & btn) == BTN_NONE) {
		pad.just_pressed |= btn;
		pad.just_repeated |= btn;
		pad.is_pressed |= btn;
		pad.repeat_at[id] = tick + PAD_REPEAT_DELAY;
	}
}

static int button_from_code(int code, int *id) {
	     if (code == CODE_UP)       { *id = BTN_ID_DPAD_UP;    return BTN_DPAD_UP; }
	else if (code == CODE_DOWN)     { *id = BTN_ID_DPAD_DOWN;  return BTN_DPAD_DOWN; }
	else if (code == CODE_LEFT)     { *id = BTN_ID_DPAD_LEFT;  return BTN_DPAD_LEFT; }
	else if (code == CODE_RIGHT)    { *id = BTN_ID_DPAD_RIGHT; return BTN_DPAD_RIGHT; }
	else if (code == CODE_A)        { *id = BTN_ID_A;          return BTN_A; }
	else if (code == CODE_B)        { *id = BTN_ID_B;          return BTN_B; }
	else if (code == CODE_X)        { *id = BTN_ID_X;          return BTN_X; }
	else if (code == CODE_Y)        { *id = BTN_ID_Y;          return BTN_Y; }
	else if (code == CODE_START)    { *id = BTN_ID_START;      return BTN_START; }
	else if (code == CODE_SELECT)   { *id = BTN_ID_SELECT;     return BTN_SELECT; }
	else if (code == CODE_MENU)     { *id = BTN_ID_MENU;       return BTN_MENU; }
	// NOTE: CODE_MENU_ALT (354/KEY_GOTO) is deliberately NOT mapped to BTN_MENU.
	// The H700 firmware reports the physical MENU button faithfully on CODE_MENU
	// (312/BTN_TL2) — down while held, up on release. On a *short* tap it also
	// emits a synthetic KEY_GOTO pulse that starts the instant CODE_MENU releases
	// and lasts ~190ms. Mapping both to BTN_MENU stretches every tap past the
	// 250ms long-press threshold, so a quick MENU tap wrongly registers as a hold
	// (brightness overlay instead of the shortcuts overlay). Ignore the synthetic
	// pulse and let NextUI derive tap-vs-hold from the clean CODE_MENU timing.
	else if (code == CODE_L1)       { *id = BTN_ID_L1;         return BTN_L1; }
	else if (code == CODE_L2)       { *id = BTN_ID_L2;         return BTN_L2; }
	else if (code == CODE_L3)       { *id = BTN_ID_L3;         return BTN_L3; }
	else if (code == CODE_R1)       { *id = BTN_ID_R1;         return BTN_R1; }
	else if (code == CODE_R2)       { *id = BTN_ID_R2;         return BTN_R2; }
	else if (code == CODE_R3)       { *id = BTN_ID_R3;         return BTN_R3; }
	else if (code == CODE_PLUS)     { *id = BTN_ID_PLUS;       return BTN_PLUS; }
	else if (code == CODE_MINUS)    { *id = BTN_ID_MINUS;      return BTN_MINUS; }
	else if (code == CODE_POWER)    { *id = BTN_ID_POWER;      return BTN_POWER; }
	return BTN_NONE;
}

static int button_from_joy(int joy, int *id) {
	     if (joy == JOY_UP)       { *id = BTN_ID_DPAD_UP;    return BTN_DPAD_UP; }
	else if (joy == JOY_DOWN)     { *id = BTN_ID_DPAD_DOWN;  return BTN_DPAD_DOWN; }
	else if (joy == JOY_LEFT)     { *id = BTN_ID_DPAD_LEFT;  return BTN_DPAD_LEFT; }
	else if (joy == JOY_RIGHT)    { *id = BTN_ID_DPAD_RIGHT; return BTN_DPAD_RIGHT; }
	else if (joy == JOY_A)        { *id = BTN_ID_A;          return BTN_A; }
	else if (joy == JOY_B)        { *id = BTN_ID_B;          return BTN_B; }
	else if (joy == JOY_X)        { *id = BTN_ID_X;          return BTN_X; }
	else if (joy == JOY_Y)        { *id = BTN_ID_Y;          return BTN_Y; }
	else if (joy == JOY_START)    { *id = BTN_ID_START;      return BTN_START; }
	else if (joy == JOY_SELECT)   { *id = BTN_ID_SELECT;     return BTN_SELECT; }
	else if (joy == JOY_MENU)     { *id = BTN_ID_MENU;       return BTN_MENU; }
	else if (joy == JOY_MENU_ALT) { *id = BTN_ID_MENU;       return BTN_MENU; }
	else if (joy == JOY_MENU_ALT2){ *id = BTN_ID_MENU;       return BTN_MENU; }
	else if (joy == JOY_L1)       { *id = BTN_ID_L1;         return BTN_L1; }
	else if (joy == JOY_L2)       { *id = BTN_ID_L2;         return BTN_L2; }
	else if (joy == JOY_L3)       { *id = BTN_ID_L3;         return BTN_L3; }
	else if (joy == JOY_R1)       { *id = BTN_ID_R1;         return BTN_R1; }
	else if (joy == JOY_R2)       { *id = BTN_ID_R2;         return BTN_R2; }
	else if (joy == JOY_R3)       { *id = BTN_ID_R3;         return BTN_R3; }
	else if (joy == JOY_PLUS)     { *id = BTN_ID_PLUS;       return BTN_PLUS; }
	else if (joy == JOY_MINUS)    { *id = BTN_ID_MINUS;      return BTN_MINUS; }
	else if (joy == JOY_POWER)    { *id = BTN_ID_POWER;      return BTN_POWER; }
	return BTN_NONE;
}

static void apply_hat_axis(int neg_id, int pos_id, int value, uint32_t tick) {
	apply_button_state(1 << neg_id, neg_id, value < 0, tick);
	apply_button_state(1 << pos_id, pos_id, value > 0, tick);
}

static void detect_device(void) {
	char *device = getenv("DEVICE");

	static const struct {
		const char *model;
		double fps;
	} panel_rates[] = {
		{ "rg28xx",     57.7317 }, // Measured, matches DTB
		{ "rg34xx",     59.155 },  // DTB-derived
		{ "rg34xxsp",   59.5691 }, // Measured
		{ "rg35xxplus", 59.256 },  // DTB-derived
		{ "rg35xxh",    59.032 },  // DTB-derived
		{ "rg35xxpro",  59.032 },  // DTB-derived
		{ "rg35xxsp",   59.524 },  // DTB-derived
		{ "rg40xxh",    59.710 },  // DTB-derived
		{ "rg40xxv",    59.7104 }, // Measured, matches DTB
		{ "rgcubexx",   59.593 },  // DTB-derived
		{ "rgsp",       59.5691 }, // Measured
	};
	panel_fps = 60.0;
	for (size_t i = 0; i < sizeof(panel_rates) / sizeof(panel_rates[0]); i++) {
		if (exactMatch(panel_rates[i].model, device)) {
			panel_fps = panel_rates[i].fps;
			break;
		}
	}

	if (!device) device = "rg35xxplus";

	// App framebuffer size. RG28XX's panel is physically 480x640 portrait, but
	// SDL_ROTATION presents a 640x480 landscape app space — keep FIXED_* there
	// and set needs_portrait_sdl for the driver/bootlogo.
	panel_w = 640;
	panel_h = 480;
	needs_portrait_sdl = exactMatch("rg28xx", device);
	if (exactMatch("rg34xx", device) || exactMatch("rg34xxsp", device)
		|| exactMatch("rgsp", device)) {
		panel_w = 720;
		panel_h = 480;
	} else if (exactMatch("rgcubexx", device)) {
		panel_w = 720;
		panel_h = 720;
	}

	// RGB LEDs: rg40xxv (1 bank), rg40xxh / rgcubexx (2 banks). Require MCU.
	int is_rg40xxv = exactMatch("rg40xxv", device);
	int model_has_rgb = is_rg40xxv || exactMatch("rg40xxh", device)
		|| exactMatch("rgcubexx", device);
	dev_has_rgb = model_has_rgb && exists(H700_LED_TTY) && exists(H700_MCU_PWR);
	dev_num_leds = !dev_has_rgb ? 0 : (is_rg40xxv ? 1 : 2);

	// Analog sticks per SKU; every stick clicks (L3 / R3).
	dev_has_lstick = 0;
	dev_has_rstick = 0;
	if (is_rg40xxv) {
		dev_has_lstick = 1;
	} else if (exactMatch("rg40xxh", device) || exactMatch("rgcubexx", device)
		|| exactMatch("rg34xxsp", device) || exactMatch("rg35xxh", device)
		|| exactMatch("rg35xxpro", device)) {
		dev_has_lstick = dev_has_rstick = 1;
	}
}

void PLAT_initPlatform(void) {
	detect_device();

	// NOTE: should_rotate must stay 0 even on the RG28XX. Its portrait panel is
	// handled entirely by the mali SDL driver (SDL_ROTATION=1 in launch.sh), so
	// the app-side coordinate space is plain 640x480 landscape; setting the flag
	// makes setRectToAspectRatio() swap axes a second time and break minarch's
	// Aspect/Fullscreen scaling.
	// On HDMI the fb is 1280x720 landscape, so that rotation must be off; this
	// overrides the launch.sh export for the lifetime of this process.
	if (needs_portrait_sdl)
		setenv("SDL_ROTATION", hdmi_active ? "0" : "1", 1);
}

static SDL_Joystick **joysticks = NULL;
static int num_joysticks = 0;

// The built-in gpio-keys pad is handled via raw evdev (above); opening it as an
// SDL joystick too would feed poll_sdl_input() duplicate events interpreted with
// the Bluetooth-pad JOY_* layout (and the pad's SDL button indices are shifted
// by its ESC/VOL keys anyway). SDL joysticks are for external pads only.
static int is_builtin_pad(const char *name) {
	return name && strcmp(name, "ANBERNIC-keys") == 0;
}

void PLAT_initInput(void) {
	detect_device();
	for (int i = 0; i < H700_INPUT_COUNT; i++)
		input_fds[i] = -1;
	last_input_scan = 0;

	if(SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0)
		LOG_error("Failed initializing joysticks: %s\n", SDL_GetError());
	SDL_JoystickEventState(SDL_ENABLE);
	int total = SDL_NumJoysticks();
    if (total > 0) {
        joysticks = (SDL_Joystick **)malloc(sizeof(SDL_Joystick *) * total);
        for (int i = 0; i < total; i++) {
			const char *name = SDL_JoystickNameForIndex(i);
			if (is_builtin_pad(name)) {
				LOG_info("Skipping built-in joystick %d: %s (handled via evdev)\n", i, name);
				continue;
			}
			SDL_Joystick *joy = SDL_JoystickOpen(i);
			if (!joy) {
				LOG_error("Failed to open joystick %d: %s\n", i, SDL_GetError());
				continue;
			}
			joysticks[num_joysticks++] = joy;
			LOG_info("Opening joystick %d: %s\n", i, SDL_JoystickName(joy));
        }
    }
	scan_evdev_inputs();
}

void PLAT_quitInput(void) {
	for (int i = 0; i < H700_INPUT_COUNT; i++)
		close_evdev_input(i);

	if (joysticks) {
        for (int i = 0; i < num_joysticks; i++) {
            if (SDL_JoystickGetAttached(joysticks[i])) {
				LOG_info("Closing joystick %d: %s\n", i, SDL_JoystickName(joysticks[i]));
				SDL_JoystickClose(joysticks[i]);
			}
        }
        free(joysticks);
        joysticks = NULL;
        num_joysticks = 0;
	}
	if (wake_fd >= 0) {
		close(wake_fd);
		wake_fd = -1;
	}
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

static void poll_sdl_input(uint32_t tick) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		int btn = BTN_NONE;
		int pressed = 0;
		int id = -1;

		if (event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) {
			pressed = event.type == SDL_JOYBUTTONDOWN;
			btn = button_from_joy(event.jbutton.button, &id);
		}
		else if (event.type == SDL_JOYHATMOTION) {
			int hat = event.jhat.value;
			apply_button_state(BTN_DPAD_UP, BTN_ID_DPAD_UP, hat & SDL_HAT_UP, tick);
			apply_button_state(BTN_DPAD_DOWN, BTN_ID_DPAD_DOWN, hat & SDL_HAT_DOWN, tick);
			apply_button_state(BTN_DPAD_LEFT, BTN_ID_DPAD_LEFT, hat & SDL_HAT_LEFT, tick);
			apply_button_state(BTN_DPAD_RIGHT, BTN_ID_DPAD_RIGHT, hat & SDL_HAT_RIGHT, tick);
			continue;
		}
		else if (event.type == SDL_JOYAXISMOTION) {
			int axis = event.jaxis.axis;
			int val = event.jaxis.value;

			if (axis == AXIS_L2) {
				btn = BTN_L2;
				id = BTN_ID_L2;
				pressed = val > 0;
			}
			else if (axis == AXIS_R2) {
				btn = BTN_R2;
				id = BTN_ID_R2;
				pressed = val > 0;
			}
			else if (axis == AXIS_LX) {
				pad.laxis.x = val;
				PAD_setAnalog(BTN_ID_ANALOG_LEFT, BTN_ID_ANALOG_RIGHT, val, tick + PAD_REPEAT_DELAY);
				continue;
			}
			else if (axis == AXIS_LY) {
				pad.laxis.y = val;
				PAD_setAnalog(BTN_ID_ANALOG_UP, BTN_ID_ANALOG_DOWN, val, tick + PAD_REPEAT_DELAY);
				continue;
			}
			else if (axis == AXIS_RX) {
				pad.raxis.x = val;
				continue;
			}
			else if (axis == AXIS_RY) {
				pad.raxis.y = val;
				continue;
			}

			if (!pressed && btn != BTN_NONE && !(pad.is_pressed & btn))
				btn = BTN_NONE;
		}
		else if (event.type == SDL_QUIT) {
			PWR_powerOff(0);
			continue;
		}
		else if (event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED) {
			PAD_update(&event);
			continue;
		}

		apply_button_state(btn, id, pressed, tick);
	}
}

static void poll_evdev_input(uint32_t tick) {
	struct input_event event;

	for (int i = 0; i < H700_INPUT_COUNT; i++) {
		int input = input_fds[i];
		if (input < 0)
			continue;

		errno = 0;
		while (read(input, &event, sizeof(event)) == sizeof(event)) {
			if (event.type != EV_KEY && event.type != EV_ABS)
				continue;

			int btn = BTN_NONE;
			int pressed = 0;
			int id = -1;
			int code = event.code;
			int value = event.value;

			if (event.type == EV_KEY) {
				if (value > 1)
					continue;
				pressed = value;
				btn = button_from_code(code, &id);
			}
			else if (event.type == EV_ABS) {
				if (code == RAW_HATY) {
					apply_hat_axis(BTN_ID_DPAD_UP, BTN_ID_DPAD_DOWN, value, tick);
					continue;
				}
				else if (code == RAW_HATX) {
					apply_hat_axis(BTN_ID_DPAD_LEFT, BTN_ID_DPAD_RIGHT, value, tick);
					continue;
				}
				else if (code == RAW_LSX) {
					pad.laxis.x = (value * 32767) / 4096;
					PAD_setAnalog(BTN_ID_ANALOG_LEFT, BTN_ID_ANALOG_RIGHT, pad.laxis.x, tick + PAD_REPEAT_DELAY);
					continue;
				}
				else if (code == RAW_LSY) {
					pad.laxis.y = (value * 32767) / 4096;
					PAD_setAnalog(BTN_ID_ANALOG_UP, BTN_ID_ANALOG_DOWN, pad.laxis.y, tick + PAD_REPEAT_DELAY);
					continue;
				}
				else if (code == RAW_RSX) {
					pad.raxis.x = (value * 32767) / 4096;
					continue;
				}
				else if (code == RAW_RSY) {
					pad.raxis.y = (value * 32767) / 4096;
					continue;
				}
			}

			apply_button_state(btn, id, pressed, tick);
		}

		if (errno && errno != EAGAIN && errno != EWOULDBLOCK)
			close_evdev_input(i);
	}
}

// wake_fd is a second, independent open of event0 used by PLAT_shouldWake.
// It must be kept drained while awake, otherwise the power-key release that
// triggers the next sleep is still buffered when PWR_waitForWake starts
// polling and the device wakes back up instantly.
static void drain_wake_fd(void) {
	if (wake_fd < 0)
		wake_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (wake_fd < 0) return;

	struct input_event event;
	errno = 0;
	while (read(wake_fd, &event, sizeof(event)) == sizeof(event));
	if (errno && errno != EAGAIN && errno != EWOULDBLOCK) {
		close(wake_fd);
		wake_fd = -1;
	}
}

void PLAT_pollInput(void) {
	pad.just_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;

	uint32_t tick = SDL_GetTicks();
	for (int i = 0; i < BTN_ID_COUNT; i++) {
		int btn = 1 << i;
		if ((pad.is_pressed & btn) && (tick >= pad.repeat_at[i])) {
			pad.just_repeated |= btn;
			pad.repeat_at[i] += PAD_REPEAT_INTERVAL;
		}
	}

	scan_evdev_inputs();
	poll_sdl_input(tick);
	poll_evdev_input(tick);

	drain_wake_fd();

	int lid_open;
	if (lid.has_lid && PLAT_lidChanged(&lid_open) && !lid_open)
		PWR_requestSleep();
}

void PLAT_updateInput(const SDL_Event *event) {
	switch (event->type) {
    case SDL_JOYDEVICEADDED: {
        int device_index = event->jdevice.which;
        const char *name = SDL_JoystickNameForIndex(device_index);
        if (is_builtin_pad(name)) {
            LOG_info("Skipping built-in joystick %d: %s (handled via evdev)\n", device_index, name);
            break;
        }
        SDL_Joystick *new_joy = SDL_JoystickOpen(device_index);
        if (new_joy) {
            joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * (num_joysticks + 1));
            joysticks[num_joysticks++] = new_joy;
            LOG_info("Joystick added at index %d: %s\n", device_index, SDL_JoystickName(new_joy));
        } else {
            LOG_error("Failed to open added joystick at index %d: %s\n", device_index, SDL_GetError());
        }
        break;
    }

    case SDL_JOYDEVICEREMOVED: {
        SDL_JoystickID removed_id = event->jdevice.which;
        for (int i = 0; i < num_joysticks; ++i) {
            if (SDL_JoystickInstanceID(joysticks[i]) == removed_id) {
                LOG_info("Joystick removed: %s\n", SDL_JoystickName(joysticks[i]));
                SDL_JoystickClose(joysticks[i]);

                // Shift down the remaining entries
                for (int j = i; j < num_joysticks - 1; ++j)
                    joysticks[j] = joysticks[j + 1];
                num_joysticks--;

                if (num_joysticks == 0) {
                    free(joysticks);
                    joysticks = NULL;
                } else {
                    joysticks = realloc(joysticks, sizeof(SDL_Joystick *) * num_joysticks);
                }
                break;
            }
        }
        break;
    }

    default:
        break;
    }
}

void PLAT_getBatteryStatus(int* is_charging, int* charge) {
	PLAT_getBatteryStatusFine(is_charging, charge);

	// worry less about battery and more about the game you're playing
	     if (*charge>80) *charge = 100;
	else if (*charge>60) *charge =  80;
	else if (*charge>40) *charge =  60;
	else if (*charge>20) *charge =  40;
	else if (*charge>10) *charge =  20;
	else           		 *charge =  10;
}

void PLAT_getCPUTemp() {
	perf.cpu_temp = getInt("/sys/devices/virtual/thermal/thermal_zone0/temp")/1000;
}

void PLAT_getCPUSpeed()
{
	perf.cpu_speed = getInt("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq")/1000;
}

void PLAT_getGPUTemp() {
	perf.gpu_temp = getInt("/sys/devices/virtual/thermal/thermal_zone1/temp")/1000;
}

void PLAT_getGPUSpeed() {
	static char* path = NULL;
	static int resolved = 0;

	if (!resolved) {
		static char* candidates[] = {
			"/sys/class/devfreq/gpu/cur_freq",
			"/sys/devices/platform/gpu/devfreq/gpu/cur_freq",
			"/sys/devices/platform/soc@03000000/1800000.gpu/devfreq/1800000.gpu/cur_freq",
			"/sys/devices/platform/soc/1800000.gpu/devfreq/1800000.gpu/cur_freq",
			"/sys/kernel/debug/clk/gpu0/clk_rate",
			"/sys/kernel/debug/clk/pll_gpu/clk_rate",
			NULL,
		};
		for (int i = 0; candidates[i]; i++) {
			if (access(candidates[i], R_OK) == 0) {
				path = candidates[i];
				break;
			}
		}
		resolved = 1;
	}

	int speed = path ? getInt(path) : 0;
	perf.gpu_speed = speed > 0 ? speed / 1000000 : 648; // MHz
}

static struct WIFI_connection connection = {
	.valid = false,
	.freq = -1,
	.link_speed = -1,
	.noise = -1,
	.rssi = -1,
	.ip = {0},
	.ssid = {0},
};

static inline void connection_reset(struct WIFI_connection *connection_info)
{
	connection_info->valid = false;
	connection_info->freq = -1;
	connection_info->link_speed = -1;
	connection_info->noise = -1;
	connection_info->rssi = -1;
	*connection_info->ip = '\0';
	*connection_info->ssid = '\0';
}

static bool bluetoothConnected = false;

void PLAT_getNetworkStatus(int* is_online)
{
	if(WIFI_enabled())
		WIFI_connectionInfo(&connection);
	else
		connection_reset(&connection);
	
	if(is_online)
		*is_online = (connection.valid && connection.ssid[0] != '\0');
	
	if(BT_enabled()) {
		bluetoothConnected = PLAT_bluetoothConnected();
	}
	else
		bluetoothConnected = false;
}

void PLAT_getBatteryStatusFine(int *is_charging, int *charge)
{	
	if(is_charging) {
		int time_to_full = getInt("/sys/class/power_supply/axp2202-battery/time_to_full_now");
		int charger_present = getInt("/sys/class/power_supply/axp2202-usb/online"); 
		*is_charging = (charger_present == 1) && (time_to_full > 0);
	}
	if(charge) {
		*charge = getInt("/sys/class/power_supply/axp2202-battery/capacity");
	}
}

int PLAT_isUSBConnected(void)
{
	// The UDC (USB Device Controller) reports "configured" once a host has
	// enumerated us as a USB gadget. This is independent of merely being
	// plugged into a charger, so it lets us tell "connected to a computer"
	// apart from "connected to power".
	DIR *dir = opendir("/sys/class/udc");
	if (!dir)
		return 0;

	int connected = 0;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL)
	{
		if (entry->d_name[0] == '.')
			continue;

		char path[512];
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", entry->d_name);

		char state[32] = {0};
		getFile(path, state, sizeof(state));
		if (strncmp(state, "configured", 10) == 0)
		{
			connected = 1;
			break;
		}
	}

	closedir(dir);
	return connected;
}

void PLAT_enableBacklight(int enable) {
	// The power LED follows sleep/wake even when HDMI owns the display.
	putInt("/sys/class/power_supply/axp2202-battery/work_led", enable ? 1 : 0);
	if (GetHDMI()) return;

	if (enable) {
		putInt("/sys/class/graphics/fb0/blank", 0);
		SetBrightness(GetBrightness());
		// the suspend script drops mcu_pwr while we sleep, so the MCU comes
		// back blank -- forget the cached frame so the next commit repaints it
		LED_invalidateCache();
	}
	else {
		SetRawBrightness(0);
		putInt("/sys/class/graphics/fb0/blank", 1);
	}
}

void PLAT_powerOff(int reboot) {
	if (CFG_getHaptics()) {
		VIB_singlePulse(VIB_bootStrength, VIB_bootDuration_ms);
	}
	system("rm -f /tmp/nextui_exec && sync");
	sleep(2);

	SetRawVolume(MUTE_VOLUME_RAW);
	PLAT_enableBacklight(0);
	LED_shutdown();
	SND_quit();
	VIB_quit();
	PWR_quit();
	GFX_quit();

	system("cat /dev/zero > /dev/fb0 2>/dev/null");
	if(reboot > 0)
		touch("/tmp/reboot");
	else
		touch("/tmp/poweroff");
	sync();
	exit(0);
}

int PLAT_supportsDeepSleep(void) { return 1; }

#define LID_PATH "/sys/class/power_supply/axp2202-battery/hallkey"

void PLAT_initLid(void) {
	lid.has_lid = exists(LID_PATH);
	if (lid.has_lid)
		lid.is_open = getInt(LID_PATH);
}

int PLAT_lidChanged(int* state) {
	if (!lid.has_lid) return 0;

	int lid_open = getInt(LID_PATH);
	if (lid_open != lid.is_open) {
		lid.is_open = lid_open;
		if (state) *state = lid_open;
		return 1;
	}
	return 0;
}

int PLAT_shouldWake(void) {
	int lid_open = 1;
	if (lid.has_lid && PLAT_lidChanged(&lid_open) && lid_open)
		return 1;

	if (wake_fd < 0)
		wake_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (wake_fd < 0) return 0;

	struct input_event event;
	int should_wake = 0;
	errno = 0;
	while (read(wake_fd, &event, sizeof(event)) == sizeof(event)) {
		if (event.type == EV_KEY && event.code == CODE_POWER && event.value == 0) {
			if (lid.has_lid && !lid.is_open) {
				should_wake = 0;
				continue;
			}
			LOG_debug("PLAT_shouldWake: power key release, waking\n");
			should_wake = 1;
		}
	}
	if (errno && errno != EAGAIN && errno != EWOULDBLOCK) {
		close(wake_fd);
		wake_fd = -1;
	}
	return should_wake;
}

///////////////////////////////

void PLAT_setCPUSpeed(int speed) {
	const char* mode;
	switch (speed) {
		case CPU_SPEED_AUTO: mode = "auto"; break;
		case CPU_SPEED_PERFORMANCE: mode = "performance"; break;
		case CPU_SPEED_POWERSAVE: mode = "powersave"; break;
		default: return;
	}
	
	const char* system_path = getenv("SYSTEM_PATH");
	if (!system_path) {
		LOG_info("WARNING: SYSTEM_PATH not set, cannot run governor script\n");
		return;
	}
	char cmd[512];
	int n = snprintf(cmd, sizeof(cmd), "sh \"%s/bin/governor.sh\" \"%s\"", system_path, mode);
	if (n < 0 || n >= (int)sizeof(cmd)) {
		LOG_info("WARNING: SYSTEM_PATH too long for governor script path\n");
		return;
	}
	int ret = system(cmd);
	if (ret != 0) LOG_info("WARNING: governor script exited with status %d for mode '%s'\n", ret, mode);
}

#define MAX_STRENGTH 0xFFFF
#define MIN_VOLTAGE 500000
#define MAX_VOLTAGE 3300000
#define RUMBLE_PATH "/sys/class/power_supply/axp2202-battery/moto"

void PLAT_setRumble(int strength) {
	putInt(RUMBLE_PATH, (strength) ? 1 : 0);
}

int PLAT_pickSampleRate(int requested, int max) {
	// bluetooth: allow limiting the maximum to improve compatibility
	// NOTE: called from SND_init before InitSettings, so msettings shared
	// memory (GetAudioSink) must not be touched here
	if(PLAT_bluetoothConnected())
		return MIN(requested, CFG_getBluetoothSamplingrateLimit());

	return MIN(requested, max);
}

void PLAT_overrideMute(int mute) {
	system(mute ? "amixer -q sset 'SPK' off" : "amixer -q sset 'SPK' on");
}

char* PLAT_getModel(void) {
	static char model_buf[64];
	char* model = getenv("RGXX_MODEL");
	if (model) {
		snprintf(model_buf, sizeof(model_buf), "%s", model);
		return model_buf;
	}
	return "Anbernic RG XX";
}

void PLAT_getOsVersionInfo(char* output_str, size_t max_len)
{
	char os_release[512] = {0};
	char kernel[128] = {0};
	FILE *fp = popen(". /etc/os-release 2>/dev/null; printf '%s' \"$PRETTY_NAME\"", "r");
	if (fp) {
		fgets(os_release, sizeof(os_release), fp);
		pclose(fp);
	}
	getFile("/proc/sys/kernel/osrelease", kernel, sizeof(kernel));
	trimTrailingNewlines(os_release);
	trimTrailingNewlines(kernel);
	snprintf(output_str, max_len, "%s%s%s", os_release[0] ? os_release : "Anbernic stock OS", kernel[0] ? " / Linux " : "", kernel);
}

bool PLAT_btIsConnected(void)
{
	return bluetoothConnected;
}

ConnectionStrength PLAT_connectionStrength(void) {
	if(!WIFI_enabled() || !connection.valid || connection.rssi == -1)
		return SIGNAL_STRENGTH_OFF;
	else if (connection.rssi == 0)
		return SIGNAL_STRENGTH_DISCONNECTED;
	else if (connection.rssi >= -60)
		return SIGNAL_STRENGTH_HIGH;
	else if (connection.rssi >= -70)
		return SIGNAL_STRENGTH_MED;
	else
		return SIGNAL_STRENGTH_LOW;
}

// LED support lives in led.c, included at the bottom of this file.

//////////////////////////////////////////////

int PLAT_setDateTime(int y, int m, int d, int h, int i, int s) {
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock -u -w", y,m,d,h,i,s);
	system(cmd);
	return 0; // why does this return an int?
}

#define MAX_LINE_LENGTH 200
#define ZONE_PATH "/usr/share/zoneinfo"
#define ZONE_TAB_PATH ZONE_PATH "/zone.tab"

static char cached_timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
static int cached_tz_count = -1;

int compare_timezones(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void PLAT_initTimezones() {
    if (cached_tz_count != -1) { // Already initialized
        return;
    }
    
    FILE *file = fopen(ZONE_TAB_PATH, "r");
    if (!file) {
        LOG_info("Error opening file %s\n", ZONE_TAB_PATH);
        return;
    }
    
    char line[MAX_LINE_LENGTH];
    cached_tz_count = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // Skip comment lines
        if (line[0] == '#' || strlen(line) < 3) {
            continue;
        }
        
        char *token = strtok(line, "\t"); // Skip country code
        if (!token) continue;
        
        token = strtok(NULL, "\t"); // Skip latitude/longitude
        if (!token) continue;
        
        token = strtok(NULL, "\t\n"); // Extract timezone
        if (!token) continue;
        
        // Check for duplicates before adding
        int duplicate = 0;
        for (int i = 0; i < cached_tz_count; i++) {
            if (strcmp(cached_timezones[i], token) == 0) {
                duplicate = 1;
                break;
            }
        }
        
        if (!duplicate && cached_tz_count < MAX_TIMEZONES) {
            strncpy(cached_timezones[cached_tz_count], token, MAX_TZ_LENGTH - 1);
            cached_timezones[cached_tz_count][MAX_TZ_LENGTH - 1] = '\0'; // Ensure null-termination
            cached_tz_count++;
        }
    }
    
    fclose(file);
    
    // Sort the list alphabetically
    qsort(cached_timezones, cached_tz_count, MAX_TZ_LENGTH, compare_timezones);
}

void PLAT_getTimezones(char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH], int *tz_count) {
    if (cached_tz_count == -1) {
        LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
        *tz_count = 0;
        return;
    }
    
    memcpy(timezones, cached_timezones, sizeof(cached_timezones));
    *tz_count = cached_tz_count;
}

char *PLAT_getCurrentTimezone() {
	char *output = (char *)malloc(256);
	if (!output) {
		return NULL;
	}
	FILE *fp = popen("timedatectl show -p Timezone --value 2>/dev/null", "r");
	if (!fp) {
		free(output);
		return NULL;
	}
	fgets(output, 256, fp);
	pclose(fp);
	trimTrailingNewlines(output);

	return output;
}

void PLAT_setCurrentTimezone(const char* tz) {
	if (cached_tz_count == -1) {
		LOG_warn("Error: Timezones not initialized. Call PLAT_initTimezones first.\n");
        return;
    }

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "timedatectl set-timezone '%s'", tz);
	system(cmd);
}

bool PLAT_getNetworkTimeSync(void) {
	char output[16] = {0};
	FILE *fp = popen("timedatectl show -p NTP --value 2>/dev/null", "r");
	if (!fp) {
		return false;
	}
	fgets(output, sizeof(output), fp);
	pclose(fp);
	return output[0] == 'y' || output[0] == 'Y';
}

void PLAT_setNetworkTimeSync(bool on) {
	system(on ? "timedatectl set-ntp true" : "timedatectl set-ntp false");
}

/////////////////////////

// We use the generic video implementation here
#include "generic_video.c"

/////////////////////////

// We use the generic wifi implementation here
#define WIFI_SOCK_DIR "/tmp/wifi/sockets"
#include "generic_wifi.c"

/////////////////////////

// We use the generic bluetooth implementation here
#include "generic_bt.c"

/////////////////////////

// RGB LEDs (RG40XX H/V and RG CubeXX only)
#include "led.c"

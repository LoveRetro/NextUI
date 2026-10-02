#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/time.h>

#include <msettings.h>

#define VOLUME_MIN 		0
#define VOLUME_MAX 		20
#define BRIGHTNESS_MIN 	0
#define BRIGHTNESS_MAX 	10
#define COLORTEMP_MIN 	0
#define COLORTEMP_MAX 	40

#define CODE_MENU		312
#define CODE_MENU_ALT	354
#define CODE_SELECT		310
#define CODE_PLUS		115
#define CODE_MINUS		114

#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define INPUT_COUNT 3

static int inputs[INPUT_COUNT] = {};
static volatile int quit = 0;

static void on_term(int sig) {
	(void)sig;
	quit = 1;
}

static uint32_t now_ms(void) {
	struct timeval tod;
	gettimeofday(&tod, NULL);
	return tod.tv_sec * 1000 + tod.tv_usec / 1000;
}

static void adjust_up(int menu_pressed, int select_pressed) {
	uint32_t val;

	if (menu_pressed) {
		val = GetBrightness();
		if (val < BRIGHTNESS_MAX) SetBrightness(++val);
	}
	else if (select_pressed) {
		val = GetColortemp();
		if (val < COLORTEMP_MAX) SetColortemp(++val);
	}
	else {
		val = GetVolume();
		if (val < VOLUME_MAX) SetVolume(++val);
	}
}

static void adjust_down(int menu_pressed, int select_pressed) {
	uint32_t val;

	if (menu_pressed) {
		val = GetBrightness();
		if (val > BRIGHTNESS_MIN) SetBrightness(--val);
	}
	else if (select_pressed) {
		val = GetColortemp();
		if (val > COLORTEMP_MIN) SetColortemp(--val);
	}
	else {
		val = GetVolume();
		if (val > VOLUME_MIN) SetVolume(--val);
	}
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	InitSettings();

	char path[32];
	for (int i = 0; i < INPUT_COUNT; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}

	struct input_event ev;
	uint32_t then = now_ms();
	uint32_t up_repeat_at = 0;
	uint32_t down_repeat_at = 0;
	int menu_pressed = 0;
	int select_pressed = 0;
	int up_pressed = 0;
	int up_just_pressed = 0;
	int down_pressed = 0;
	int down_just_pressed = 0;

	while (!quit) {
		uint32_t now = now_ms();
		int ignore = (now - then) > 1000;

		for (int i = 0; i < INPUT_COUNT; i++) {
			if (inputs[i] < 0) continue;

			while (read(inputs[i], &ev, sizeof(ev)) == sizeof(ev)) {
				if (ignore) continue;
				if (ev.type != EV_KEY || ev.value > REPEAT) continue;

				switch (ev.code) {
					case CODE_MENU:
						// Only the real MENU code (312) tracks the physical button.
						// CODE_MENU_ALT (354/KEY_GOTO) is a synthetic post-tap pulse
						// (see platform.c) and must not be treated as MENU held, or a
						// stray tap could momentarily divert vol +/- to brightness.
						menu_pressed = ev.value;
					break;
					case CODE_SELECT:
						select_pressed = ev.value;
					break;
					case CODE_PLUS:
						up_pressed = up_just_pressed = ev.value;
						if (ev.value) up_repeat_at = now + 300;
					break;
					case CODE_MINUS:
						down_pressed = down_just_pressed = ev.value;
						if (ev.value) down_repeat_at = now + 300;
					break;
					default:
					break;
				}
			}
		}

		if (ignore) {
			menu_pressed = 0;
			select_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}

		if (up_just_pressed || (up_pressed && now >= up_repeat_at)) {
			adjust_up(menu_pressed, select_pressed);
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}

		if (down_just_pressed || (down_pressed && now >= down_repeat_at)) {
			adjust_down(menu_pressed, select_pressed);
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}

		then = now;
		usleep(16666);
	}

	for (int i = 0; i < INPUT_COUNT; i++)
		if (inputs[i] >= 0) close(inputs[i]);

	QuitSettings();
	return 0;
}

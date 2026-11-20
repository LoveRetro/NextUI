#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

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
static volatile sig_atomic_t quit = 0;
static volatile sig_atomic_t resumed = 0;

static void on_term(int sig) {
	(void)sig;
	quit = 1;
}

// PWR_enterSleep() SIGSTOPs keymon; input queued until SIGCONT must be dropped.
static void on_cont(int sig) {
	(void)sig;
	resumed = 1;
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ms_until(uint32_t at, uint32_t now) {
	int32_t diff = (int32_t)(at - now);
	return diff > 0 ? diff : 0;
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
	sa.sa_handler = on_cont;
	sigaction(SIGCONT, &sa, NULL);

	InitSettings();

	// Signals stay blocked except inside ppoll() so none is lost between the
	// flag checks and the wait.
	sigset_t blocked, waitmask;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGCONT);
	sigprocmask(SIG_BLOCK, &blocked, &waitmask);
	sigset_t continued;
	sigemptyset(&continued);
	sigaddset(&continued, SIGCONT);
	const struct timespec no_wait = {0};

	char path[32];
	struct pollfd fds[INPUT_COUNT];
	for (int i = 0; i < INPUT_COUNT; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		fds[i].fd = inputs[i];
		fds[i].events = POLLIN;
	}

	struct input_event ev;
	uint32_t up_repeat_at = 0;
	uint32_t down_repeat_at = 0;
	int menu_pressed = 0;
	int select_pressed = 0;
	int up_pressed = 0;
	int up_just_pressed = 0;
	int down_pressed = 0;
	int down_just_pressed = 0;

	while (!quit) {
		int timeout = -1;
		uint32_t now = now_ms();
		if (up_pressed) timeout = ms_until(up_repeat_at, now);
		if (down_pressed) {
			int t = ms_until(down_repeat_at, now);
			if (timeout < 0 || t < timeout) timeout = t;
		}
		struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000000L };
		ppoll(fds, INPUT_COUNT, timeout < 0 ? NULL : &ts, &waitmask);
		if (quit) break;
		for (int i = 0; i < INPUT_COUNT; i++)
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) fds[i].fd = -1; // gone; would wake poll forever

		now = now_ms();
		int ignore = resumed;
		int got_input = 0;
		resumed = 0;

		for (int i = 0; i < INPUT_COUNT; i++) {
			if (inputs[i] < 0) continue;

			while (read(inputs[i], &ev, sizeof(ev)) == sizeof(ev)) {
				got_input = 1;
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

		// A stop can land between ppoll and these reads. Consume a pending resume
		// after draining the descriptors, before applying any of their events.
		while (sigtimedwait(&continued, NULL, &no_wait) == SIGCONT) {
			ignore = 1;
			for (int i = 0; i < INPUT_COUNT; i++)
				while (read(inputs[i], &ev, sizeof(ev)) == sizeof(ev)) {}
		}
		if (ignore) {
			menu_pressed = 0;
			select_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}

		if (up_just_pressed || (up_pressed && ms_until(up_repeat_at, now) == 0)) {
			adjust_up(menu_pressed, select_pressed);
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}

		if (down_just_pressed || (down_pressed && ms_until(down_repeat_at, now) == 0)) {
			adjust_down(menu_pressed, select_pressed);
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}

		// Batch input bursts (analog sticks, autorepeat) at the old 60 Hz rate.
		if (got_input) usleep(16666);
	}

	for (int i = 0; i < INPUT_COUNT; i++)
		if (inputs[i] >= 0) close(inputs[i]);

	QuitSettings();
	return 0;
}

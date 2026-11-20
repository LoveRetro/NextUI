#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#include <msettings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// #include "defines.h"

#define VOLUME_MIN 		0
#define VOLUME_MAX 		20
#define BRIGHTNESS_MIN 	0
#define BRIGHTNESS_MAX 	10
#define COLORTEMP_MIN 	0
#define COLORTEMP_MAX 	40

#define CODE_MENU0		314
#define CODE_MENU1		315
#define CODE_MENU2		316
#define CODE_PLUS		115
#define CODE_MINUS		114
#define CODE_MUTE		1
#define CODE_JACK		2

// keymon and api might need different codes

//	for ev.value
#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define MUTE_STATE_PATH "/sys/class/gpio/gpio243/value"

#define INPUT_COUNT 5
static int inputs[INPUT_COUNT] = {};
static struct input_event ev;

static volatile int quit = 0;
static void on_term(int sig) { quit = 1; }
// PWR_enterSleep() SIGSTOPs keymon; input queued until SIGCONT must be dropped.
static volatile sig_atomic_t resumed = 0;
static void on_cont(int sig) { resumed = 1; }

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int ms_until(uint32_t at, uint32_t now) {
	int32_t diff = (int32_t)(at - now);
	return diff > 0 ? diff : 0;
}

static int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}

int stringEquals(const char* str1, const char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

static pthread_t mute_pt;
static void* watchMute(void *arg) {
	int is_muted,was_muted;
	int is_brickpro = stringEquals(getenv("DEVICE"), "brickpro");
	
	is_muted = was_muted = getInt(MUTE_STATE_PATH);
	SetMute(is_muted);
	
	while(!quit) {
		usleep(200000); // 5 times per second
		
		is_muted = getInt(MUTE_STATE_PATH);
		// swallow mute val -1 on shutdown
		if (is_muted >= 0 && was_muted!=is_muted) {
			was_muted = is_muted;
			SetMute(is_muted);
			if (GetMute()) {
				// tmp solution
				if (is_brickpro)
					system("echo 900000 > /sys/class/motor/voltage");
				else
					system("echo 1500000 > /sys/class/motor/voltage");
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
			}
		}
	}
	
	return NULL;
}

int main (int argc, char *argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = on_cont;
	sigaction(SIGCONT, &sa, NULL);

	InitSettings();

	// Signals stay blocked except inside ppoll() so none is lost between the
	// flag checks and the wait. Block before spawning the mute thread so it
	// inherits the mask and the signals always reach the main thread.
	sigset_t blocked, waitmask;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGCONT);
	sigprocmask(SIG_BLOCK, &blocked, &waitmask);
	sigset_t continued;
	sigemptyset(&continued);
	sigaddset(&continued, SIGCONT);
	const struct timespec no_wait = {0};

	pthread_create(&mute_pt, NULL, &watchMute, NULL);

	char path[32];
	struct pollfd fds[INPUT_COUNT];
	for (int i=0; i<INPUT_COUNT; i++) {
		sprintf(path, "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		fds[i].fd = inputs[i];
		fds[i].events = POLLIN;
	}
	
	uint32_t input;
	uint32_t val;
	uint32_t menu_pressed = 0;
	uint32_t menu2_pressed = 0;
	
	uint32_t up_pressed = 0;
	uint32_t up_just_pressed = 0;
	uint32_t up_repeat_at = 0;
	
	uint32_t down_pressed = 0;
	uint32_t down_just_pressed = 0;
	uint32_t down_repeat_at = 0;
	
	uint8_t ignore;
	uint8_t got_input;
	uint32_t now;
	
	while (!quit) {
		int timeout = -1;
		now = now_ms();
		if (up_pressed) timeout = ms_until(up_repeat_at, now);
		if (down_pressed) {
			int t = ms_until(down_repeat_at, now);
			if (timeout < 0 || t < timeout) timeout = t;
		}
		struct timespec ts = { timeout / 1000, (timeout % 1000) * 1000000L };
		ppoll(fds, INPUT_COUNT, timeout < 0 ? NULL : &ts, &waitmask);
		if (quit) break;
		for (int i=0; i<INPUT_COUNT; i++)
			if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) fds[i].fd = -1; // gone; would wake poll forever

		now = now_ms();
		ignore = resumed; // ignore input that arrived during sleep
		resumed = 0;
		got_input = 0;
		int jack = -1;
		
		for (int i=0; i<INPUT_COUNT; i++) {
			input = inputs[i];
			while(read(input, &ev, sizeof(ev))==sizeof(ev)) {
				got_input = 1;
				if (ignore) continue;
				val = ev.value;
				if (ev.type==EV_SW) {
					//printf("switch: %i\n", ev.code);
					if (ev.code==CODE_JACK) {
						//printf("jack: %i\n", val);
						jack = val;
					}
				}
				if (( ev.type != EV_KEY ) || ( val > REPEAT )) continue;
				//printf("code: %i (%i)\n", ev.code, val); fflush(stdout);
				switch (ev.code) {
					case CODE_MENU2:
						menu_pressed = val;
					break;
					case CODE_MENU0:
						menu2_pressed = val;
					break;
					case CODE_PLUS:
						up_pressed = up_just_pressed = val;
						if (val) up_repeat_at = now + 300;
					break;
					case CODE_MINUS:
						down_pressed = down_just_pressed = val;
						if (val) down_repeat_at = now + 300;
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
		if (!ignore && jack >= 0) SetJack(jack);
		if (ignore) {
			menu_pressed = 0;
			menu2_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}
		
		if (up_just_pressed || (up_pressed && ms_until(up_repeat_at, now) == 0)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val<BRIGHTNESS_MAX) SetBrightness(++val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val<COLORTEMP_MAX) {
					SetColortemp(++val);
				}
			}
			else {
				val = GetVolume();
				if (val<VOLUME_MAX) SetVolume(++val);
			}
			
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}
		
		if (down_just_pressed || (down_pressed && ms_until(down_repeat_at, now) == 0)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val>BRIGHTNESS_MIN) SetBrightness(--val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val>COLORTEMP_MIN) {
					SetColortemp(--val);
				}
			}
			else {
				val = GetVolume();
				if (val>VOLUME_MIN) SetVolume(--val);
			}
			
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}
		
		// Batch input bursts (analog sticks, autorepeat) at the old 60 Hz rate.
		if (got_input) usleep(16666);
	}

	for (int i=0; i<INPUT_COUNT; i++)
		close(inputs[i]);
	
	pthread_cancel(mute_pt);
	pthread_join(mute_pt, NULL);
}

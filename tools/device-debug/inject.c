// Virtual gamepad for driving NextUI over adb (TrimUI Brick / tg5040 layout).
//
// Creates a uinput device that looks like the built-in pad ("TRIMUI Player1",
// Xbox-style: buttons + HAT0 d-pad) and presses buttons named in a FIFO.
// Tokens: up down left right a b x y l1 r1 sleep quit; append :<ms> to hold, e.g. a:400
// See DEBUGGING.md. Button indices must match JOY_* in workspace/<platform>/platform/platform.h.
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;

static void ev(int type, int code, int value)
{
	struct input_event e;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.code = code;
	e.value = value;
	write(fd, &e, sizeof e);
}

static int hold_ms = 120;

static void tap(int type, int code, int on)
{
	ev(type, code, on);
	ev(EV_SYN, SYN_REPORT, 0);
	usleep(hold_ms * 1000);
	ev(type, code, 0);
	ev(EV_SYN, SYN_REPORT, 0);
	usleep(150000);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <fifo>\n", argv[0]);
		return 1;
	}
	fd = open("/dev/uinput", O_WRONLY);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}

	// xpad button codes in order: A B X Y TL TR SELECT START MODE THUMBL THUMBR.
	// SDL numbers joystick buttons by that order, so JOY_B=0 is 0x130, JOY_A=1 is 0x131, ...
	int keys[] = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e};
	int axes[] = {0, 1, 2, 3, 4, 5, 16, 17}; // sticks + HAT0X/HAT0Y

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int i = 0; i < 11; i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	struct uinput_user_dev d;
	memset(&d, 0, sizeof d);
	strcpy(d.name, "TRIMUI Player1");
	d.id.bustype = 3;
	d.id.vendor = 0x45e;
	d.id.product = 0x28e;
	d.id.version = 0x114;
	for (int i = 0; i < 8; i++) {
		ioctl(fd, UI_SET_ABSBIT, axes[i]);
		d.absmin[axes[i]] = axes[i] >= 16 ? -1 : -32768;
		d.absmax[axes[i]] = axes[i] >= 16 ? 1 : 32767;
	}
	write(fd, &d, sizeof d);
	ioctl(fd, UI_DEV_CREATE);
	sleep(1); // let the app notice the new joystick

	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	char tok[32];
	while (1) {
		if (fscanf(f, "%31s", tok) != 1) {
			clearerr(f);
			usleep(100000);
			continue;
		}
		// "<button>:<ms>" holds the button down for that long, e.g. "a:400"
		char *colon = strchr(tok, ':');
		hold_ms = 120;
		if (colon) {
			hold_ms = atoi(colon + 1);
			*colon = 0;
		}
		if (!strcmp(tok, "up")) tap(EV_ABS, 17, -1);
		else if (!strcmp(tok, "down")) tap(EV_ABS, 17, 1);
		else if (!strcmp(tok, "left")) tap(EV_ABS, 16, -1);
		else if (!strcmp(tok, "right")) tap(EV_ABS, 16, 1);
		else if (!strcmp(tok, "b")) tap(EV_KEY, 0x130, 1);
		else if (!strcmp(tok, "a")) tap(EV_KEY, 0x131, 1);
		else if (!strcmp(tok, "y")) tap(EV_KEY, 0x133, 1);
		else if (!strcmp(tok, "x")) tap(EV_KEY, 0x134, 1);
		else if (!strcmp(tok, "l1")) tap(EV_KEY, 0x136, 1);
		else if (!strcmp(tok, "r1")) tap(EV_KEY, 0x137, 1);
		else if (!strcmp(tok, "sleep")) sleep(1);
		else if (!strcmp(tok, "quit")) break;
	}
	ioctl(fd, UI_DEV_DESTROY);
	return 0;
}

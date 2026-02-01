/*
waitforwake.c for NextUI (tg5040)
usage:
waitforwake.elf <seconds>

return: 0 on timeout, 1 if power button pressed

This util (based on keymon.elf) exits after sleeping for x seconds,
or until the power button is pressed, whichever comes first.

This little program is called when the system is waiting to
retry a failed deep-sleep; if the user presses the power button
this program will exit immediately and notify the calling script
that the user requested a system wake-up
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include <fcntl.h>

#include <linux/input.h>

int is_number(char * s) {
	char * c = s;

	if (*c == '\0') return 0;

	do {
		if (*c < '0' || *c > '9') {
			return 0;
		}
		c++;
	} while (*c != '\0');
	return 1;
}

static char * command_name = "waitforwake.elf";

void print_usage() {
	printf("usage:\n"
		   "%s <seconds>\n"
		   "<seconds> must be 1-60\n"
		   "return values: 0 - timeout; 1 - power button pressed\n"
		   "              -1 - error\n",
		   command_name);
}

int main(int argc, char ** argv) {

	command_name = argv[0];

	if (argc < 2 || !is_number(argv[1])) {
		print_usage();
		return -1;
	}

	uint32_t secs = atoi(argv[1]);

	if (secs < 1 || secs > 60) {
		print_usage();
		return -1;
	}

	
	// You might need to change this if you're porting this
	// to a different device
	const char * INP_FILENAME = "/dev/input/event1";
	int input_fd = open(INP_FILENAME, O_RDONLY | O_NONBLOCK);

	if (input_fd < 0) {
		printf("%s: could not open %s\n"
			   "(will pause without checking power button input)\n", argv[0],
			   INP_FILENAME);
		sleep(secs);
		return 0;
	}
	
	const int CODE_PWR = 116;      // power button keycode
	const int TIMESTEP = 200;      // 5 per second (in ms)
	uint32_t ttl = secs * 1000;    // time to live (milliseconds)
	uint32_t time_running = 0;

	struct input_event ev;

	while (time_running < ttl) {
		usleep(TIMESTEP * 1000);
		time_running += TIMESTEP;

		while (read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type == EV_KEY && ev.code == CODE_PWR &&
				ev.value == 1) {
				close(input_fd);
				return 1;
			}
		}
		
	}
	close(input_fd);
 
	return 0;
}

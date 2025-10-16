#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/rfkill.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include <dirent.h>

void show_usage(const char *prog)
{
	fprintf(stderr,
			"Usage:\n"
			"  %s list [INDEX|TYPE]\n"
			"  %s block INDEX|TYPE\n"
			"  %s unblock INDEX|TYPE\n",
			prog, prog, prog);
	exit(EXIT_FAILURE);
}

int get_rfkill_type(const char *name)
{
	static const char *types[] = {"all", "wlan", "bluetooth", "uwb", "wimax", "wwan", "gps", "fm", NULL};
	if (strcmp(name, "wifi") == 0)
		name = "wlan";
	if (strcmp(name, "ultrawideband") == 0)
		name = "uwb";

	for (int i = 0; types[i]; ++i) {
		if (strcmp(name, types[i]) == 0)
			return i;
	}
	return -1;
}

void list_devices(const char *filter)
{
	int fd = open("/dev/rfkill", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/rfkill");
		exit(EXIT_FAILURE);
	}

	struct rfkill_event ev;
	ssize_t len;

	int rf_type = -1, rf_idx = -1;
	if (filter) {
		rf_type = get_rfkill_type(filter);
		if (rf_type < 0)
			rf_idx = atoi(filter);
	}

	while ((len = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
		if ((rf_type >= 0 && ev.type != rf_type) || (rf_idx >= 0 && ev.idx != rf_idx))
			continue;

		char path[128], line[128], name[64] = "", type[64] = "";
		snprintf(path, sizeof(path), "/sys/class/rfkill/rfkill%u/uevent", ev.idx);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;

		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "RFKILL_NAME=%63s", name) == 1)
				continue;
			if (sscanf(line, "RFKILL_TYPE=%63s", type) == 1)
				continue;
		}
		fclose(f);

		printf("%u: %s: %s\n", ev.idx, name, type);
		printf("\tSoft blocked: %s\n", ev.soft ? "yes" : "no");
		printf("\tHard blocked: %s\n", ev.hard ? "yes" : "no");
	}

	close(fd);
}

void change_block(const char *target, int block)
{
	int fd = open("/dev/rfkill", O_WRONLY);
	if (fd < 0) {
		perror("/dev/rfkill");
		exit(EXIT_FAILURE);
	}

	struct rfkill_event ev = {0};
	int type = get_rfkill_type(target);
	int idx = (type < 0) ? atoi(target) : -1;

	ev.op = (idx >= 0) ? RFKILL_OP_CHANGE : RFKILL_OP_CHANGE_ALL;
	ev.soft = block;
	if (type >= 0)
		ev.type = type;
	if (idx >= 0)
		ev.idx = idx;

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
		perror("write");
		close(fd);
		exit(EXIT_FAILURE);
	}

	close(fd);
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3)
		show_usage(argv[0]);

	if (strcmp(argv[1], "list") == 0) {
		list_devices(argv[2]);
	} else if (strcmp(argv[1], "block") == 0 && argc == 3) {
		change_block(argv[2], 1);
	} else if (strcmp(argv[1], "unblock") == 0 && argc == 3) {
		change_block(argv[2], 0);
	} else {
		show_usage(argv[0]);
	}

	return 0;
}
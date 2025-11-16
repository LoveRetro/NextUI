#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/reboot.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <time.h>
#include <unistd.h>

static void kill_processes(int sig)
{
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        perror("poweroff_next: opendir(/proc)");
        return;
    }

    pid_t self = getpid();
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL)
    {
        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;

        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (pid <= 1 || pid == self)
            continue;

        if (kill(pid, sig) != 0 && errno != ESRCH)
        {
            int err = errno;
            fprintf(stderr, "poweroff_next: failed to send signal %d to %d: %s\n",
                    sig, pid, strerror(err));
        }
    }

    closedir(proc);
}

static void kill_all_processes(void)
{
    puts("poweroff_next: Attempting to gracefully shut down processes...");
    kill_processes(SIGTERM);

    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 500000000, // 0.5 seconds
    };
    nanosleep(&ts, NULL);

    puts("poweroff_next: Forcing termination of remaining processes...");
    kill_processes(SIGKILL);
}

static void swapoff_device(const char *path)
{
    if (swapoff(path) != 0 && errno != ENOENT && errno != EINVAL)
    {
        int err = errno;
        fprintf(stderr, "poweroff_next: swapoff(%s) failed: %s\n", path, strerror(err));
    }
}

static void swapoff_all(void)
{
    FILE *swaps = fopen("/proc/swaps", "r");
    if (!swaps)
    {
        perror("poweroff_next: fopen(/proc/swaps)");
        return;
    }

    char line[256];
    // Skip header
    if (!fgets(line, sizeof(line), swaps))
    {
        fclose(swaps);
        return;
    }

    while (fgets(line, sizeof(line), swaps))
    {
        char device[128];
        if (sscanf(line, "%127s", device) == 1)
            swapoff_device(device);
    }

    fclose(swaps);
}

static void safe_umount(const char *path, int flags)
{
    if (umount2(path, flags) != 0)
    {
        if (errno == EINVAL || errno == ENOENT)
            return;

        int err = errno;
        fprintf(stderr, "poweroff_next: umount2(%s) failed: %s\n", path, strerror(err));
    }
}

static void finalize_poweroff(void)
{
    execlp("busybox", "busybox", "poweroff", NULL);
    execlp("poweroff", "poweroff", NULL);
    sync();
    reboot(RB_POWER_OFF);
    perror("poweroff_next poweroff");
}

int main(void)
{
    kill_all_processes();
    sync();
    swapoff_all();

    safe_umount("/etc/profile", MNT_FORCE);
    safe_umount("/mnt/SDCARD", MNT_DETACH);

    puts("poweroff_next: Shutting down the system...");
    finalize_poweroff();
    return 0;
}

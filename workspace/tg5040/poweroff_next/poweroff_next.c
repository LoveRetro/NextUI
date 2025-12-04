#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <linux/reboot.h>
#include <mntent.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "defines.h"
#include "config.h"

#define SDCARD_PREFIX SDCARD_PATH
#define I2C_DEVICE "/dev/i2c-6"
#define AXP2202_ADDR 0x34
#define LOG_FILE "/root/powerofflog.txt"

static FILE *log_fp = NULL;

static void log_msg(const char *format, ...)
{
    va_list args;
    
    // Write to log file
    if (log_fp) {
        va_start(args, format);
        vfprintf(log_fp, format, args);
        va_end(args);
        fflush(log_fp);
    }
    
    // Also write to stdout for adb shell
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

static void kill_processes(int sig)
{
    pid_t self = getpid();
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        log_msg("poweroff_next: opendir(/proc): %s\n", strerror(errno));
        return;
    }
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
            log_msg("poweroff_next: failed to send signal %d to %d: %s\n",
                    sig, pid, strerror(err));
        }
    }

    closedir(proc);
}

static void kill_all_processes(void)
{
    kill_processes(SIGTERM);

    struct timespec ts = {
        .tv_sec = 2,
        .tv_nsec = 0, // 2 seconds - give processes time to exit
    };
    nanosleep(&ts, NULL);

    kill_processes(SIGKILL);
}

static void swapoff_device(const char *path)
{
    if (swapoff(path) != 0 && errno != ENOENT && errno != EINVAL)
    {
        int err = errno;
        log_msg("poweroff_next: swapoff(%s) failed: %s\n", path, strerror(err));
    }
}

static void swapoff_all(void)
{
    FILE *swaps = fopen("/proc/swaps", "r");
    if (!swaps)
    {
        log_msg("poweroff_next: fopen(/proc/swaps): %s\n", strerror(errno));
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
        {
            swapoff_device(device);
        }
    }

    fclose(swaps);
}

static void safe_umount(const char *path, int flags)
{
    if (umount2(path, flags) != 0 && errno != EINVAL && errno != ENOENT)
    {
        int err = errno;
        log_msg("poweroff_next: umount2(%s) failed: %s\n", path, strerror(err));
    }
}

static void finalize_poweroff(void)
{
    sync();
    
    // Use the syscall directly with proper magic numbers
    // This is equivalent to kernel_power_off() in kernel space
    if (syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, 
                LINUX_REBOOT_CMD_POWER_OFF, NULL) != 0)
    {
        log_msg("poweroff_next: syscall(SYS_reboot, POWER_OFF) failed: %s\n", strerror(errno));
    }
    
    // Fallback to busybox/poweroff if syscall failed
    execlp("busybox", "busybox", "poweroff", NULL);
    execlp("poweroff", "poweroff", NULL);
    
    // Last resort - use the glibc wrapper
    reboot(RB_POWER_OFF);
    log_msg("poweroff_next: All poweroff methods failed: %s\n", strerror(errno));
}

static void kill_sdcard_users(void)
{
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        log_msg("poweroff_next: kill_sdcard_users: Failed to open /proc\n");
        return;
    }

    pid_t self = getpid();
    struct dirent *entry;
    char fd_dir_path[PATH_MAX];
    char fd_path[PATH_MAX];
    char target[PATH_MAX];

    while ((entry = readdir(proc)) != NULL)
    {
        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;

        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (pid <= 1 || pid == self)
            continue;

        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%s/fd", entry->d_name);
        DIR *fd_dir = opendir(fd_dir_path);
        if (!fd_dir)
            continue;

        struct dirent *fd_entry;
        while ((fd_entry = readdir(fd_dir)) != NULL)
        {
            if (fd_entry->d_name[0] == '.')
                continue;

            snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir_path, fd_entry->d_name);
            ssize_t len = readlink(fd_path, target, sizeof(target) - 1);
            if (len <= 0)
                continue;

            target[len] = '\0';
            if (strncmp(target, SDCARD_PREFIX, strlen(SDCARD_PREFIX)) == 0)
            {
                kill(pid, SIGKILL);
                break;
            }
        }

        closedir(fd_dir);
    }

    closedir(proc);
}

static bool is_sdcard_mounted(void)
{
    bool mounted = false;
    FILE *fp = setmntent("/proc/mounts", "r");
    if (!fp)
    {
        log_msg("poweroff_next: is_sdcard_mounted: Failed to open /proc/mounts\n");
        return false;
    }

    struct mntent *ent;
    while ((ent = getmntent(fp)) != NULL)
    {
        if (strcmp(ent->mnt_dir, SDCARD_PREFIX) == 0)
        {
            mounted = true;
            break;
        }
    }

    endmntent(fp);
    return mounted;
}

static bool unmount_sdcard_with_retries(void)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        safe_umount(SDCARD_PREFIX, MNT_FORCE | MNT_DETACH);

        struct timespec wait = {.tv_sec = 0, .tv_nsec = 800000000};
        nanosleep(&wait, NULL);

        if (!is_sdcard_mounted())
            return true;

        kill_sdcard_users();
        sync();
    }

    if (is_sdcard_mounted())
    {
        log_msg("poweroff_next: Failed to unmount %s after retries.\n", SDCARD_PREFIX);
        return false;
    }

    return true;
}

static int axp2202_write_reg(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    ssize_t bytes = write(fd, buffer, sizeof(buffer));
    int result = bytes == (ssize_t)sizeof(buffer) ? 0 : -1;
    if (result != 0)
        log_msg("poweroff_next: [DEBUG] axp2202_write_reg: Failed to write 0x%02x to reg 0x%02x\n", value, reg);
    return result;
}

static int execute_axp2202_poweroff(void)
{
    int fd = open(I2C_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        log_msg("poweroff_next: open(I2C_DEVICE): %s\n", strerror(errno));
        return -1;
    }

    // Try normal I2C_SLAVE first, then force if busy
    if (ioctl(fd, I2C_SLAVE, AXP2202_ADDR) < 0)
    {
        log_msg("poweroff_next: ioctl(I2C_SLAVE) failed: %s, trying I2C_SLAVE_FORCE\n", strerror(errno));
        if (ioctl(fd, I2C_SLAVE_FORCE, AXP2202_ADDR) < 0)
        {
            log_msg("poweroff_next: ioctl(I2C_SLAVE_FORCE): %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    for (int reg = 0x40; reg <= 0x44; ++reg)
        axp2202_write_reg(fd, (uint8_t)reg, 0x00);

    for (int reg = 0x48; reg <= 0x4C; ++reg)
        axp2202_write_reg(fd, (uint8_t)reg, 0xFF);

    axp2202_write_reg(fd, 0x22, 0x0A);
    struct timespec wait = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&wait, NULL);

    int ret = axp2202_write_reg(fd, 0x27, 0x01);
    close(fd);

    struct timespec latch = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&latch, NULL);

    return ret;
}

static int run_poweroff_protection(void)
{
    kill_sdcard_users();
    sync();
    swapoff_all();
    safe_umount("/etc/profile", MNT_FORCE);

    bool unmounted = unmount_sdcard_with_retries();
    if (!unmounted)
        log_msg("poweroff_next: SD card remained mounted after retries.\n");

    kill_all_processes();

    sync();
    
    struct timespec pre_pmic_wait = {.tv_sec = 0, .tv_nsec = 500000000};
    nanosleep(&pre_pmic_wait, NULL);

    if (execute_axp2202_poweroff() != 0)
    {
        log_msg("poweroff_next: PMIC shutdown sequence failed.\n");
        return -1;
    }

    finalize_poweroff();
    
    return 0;
}

static void run_standard_shutdown(void)
{
    kill_all_processes();
    sync();
    swapoff_all();

    safe_umount("/etc/profile", MNT_FORCE);
    safe_umount(SDCARD_PATH, MNT_DETACH);

    finalize_poweroff();
}

int main(void)
{
    // Open log file first thing
    log_fp = fopen(LOG_FILE, "w");
    if (log_fp) {
        // Make unbuffered so we don't lose messages if we crash/poweroff
        setvbuf(log_fp, NULL, _IONBF, 0);
    }

    // Block SIGTERM and SIGKILL for this process to prevent self-termination
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGTERM);
    sigaddset(&block_set, SIGINT);
    sigaddset(&block_set, SIGHUP);
    sigprocmask(SIG_BLOCK, &block_set, NULL);
    
    CFG_init(NULL, NULL);

    bool protection_enabled = CFG_getPowerOffProtection();
    log_msg("poweroff_next: [DEBUG] main: Power-off protection = %s\n", protection_enabled ? "enabled" : "disabled");

    if (protection_enabled)
    {
        if (run_poweroff_protection() == 0)
        {
            CFG_quit();
            return 0;
        }

        log_msg("poweroff_next: Falling back to standard shutdown.\n");
    }

    run_standard_shutdown();
    CFG_quit();
    return 0;
}

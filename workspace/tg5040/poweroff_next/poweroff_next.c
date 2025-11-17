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
    log_msg("poweroff_next: [DEBUG] kill_processes: Starting with signal %d (my PID=%d)\n", sig, self);
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        log_msg("poweroff_next: opendir(/proc): %s\n", strerror(errno));
        return;
    }
    struct dirent *entry;
    int killed_count = 0;
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
        else
        {
            killed_count++;
        }
    }

    closedir(proc);
    log_msg("poweroff_next: [DEBUG] kill_processes: Sent signal %d to %d processes\n", sig, killed_count);
}

static void kill_all_processes(void)
{
    log_msg("poweroff_next: Attempting to gracefully shut down processes...\n");
    kill_processes(SIGTERM);

    struct timespec ts = {
        .tv_sec = 2,
        .tv_nsec = 0, // 2 seconds - give processes time to exit
    };
    nanosleep(&ts, NULL);

    log_msg("poweroff_next: Forcing termination of remaining processes...\n");
    kill_processes(SIGKILL);
}

static void swapoff_device(const char *path)
{
    log_msg("poweroff_next: [DEBUG] swapoff_device: Attempting to swapoff %s\n", path);
    if (swapoff(path) != 0 && errno != ENOENT && errno != EINVAL)
    {
        int err = errno;
        log_msg("poweroff_next: swapoff(%s) failed: %s\n", path, strerror(err));
    }
    else
    {
        log_msg("poweroff_next: [DEBUG] swapoff_device: Successfully turned off swap on %s\n", path);
    }
}

static void swapoff_all(void)
{
    printf("poweroff_next: [DEBUG] swapoff_all: Starting\n");
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

    int swap_count = 0;
    while (fgets(line, sizeof(line), swaps))
    {
        char device[128];
        if (sscanf(line, "%127s", device) == 1)
        {
            swapoff_device(device);
            swap_count++;
        }
    }

    fclose(swaps);
    log_msg("poweroff_next: [DEBUG] swapoff_all: Processed %d swap devices\n", swap_count);
}

static void safe_umount(const char *path, int flags)
{
    log_msg("poweroff_next: [DEBUG] safe_umount: Attempting to unmount %s with flags 0x%x\n", path, flags);
    if (umount2(path, flags) != 0)
    {
        if (errno == EINVAL || errno == ENOENT)
        {
            log_msg("poweroff_next: [DEBUG] safe_umount: %s not mounted or invalid\n", path);
            return;
        }

        int err = errno;
        log_msg("poweroff_next: umount2(%s) failed: %s\n", path, strerror(err));
    }
    else
    {
        log_msg("poweroff_next: [DEBUG] safe_umount: Successfully unmounted %s\n", path);
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
    printf("poweroff_next: [DEBUG] kill_sdcard_users: Starting\n");
    DIR *proc = opendir("/proc");
    if (!proc)
    {
        printf("poweroff_next: [DEBUG] kill_sdcard_users: Failed to open /proc\n");
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
                log_msg("poweroff_next: [DEBUG] kill_sdcard_users: Killing PID %d (has fd to %s)\n", pid, target);
                kill(pid, SIGKILL);
                break;
            }
        }

        closedir(fd_dir);
    }

    closedir(proc);
    printf("poweroff_next: [DEBUG] kill_sdcard_users: Completed\n");
}

static bool is_sdcard_mounted(void)
{
    printf("poweroff_next: [DEBUG] is_sdcard_mounted: Checking\n");
    bool mounted = false;
    FILE *fp = setmntent("/proc/mounts", "r");
    if (!fp)
    {
        printf("poweroff_next: [DEBUG] is_sdcard_mounted: Failed to open /proc/mounts\n");
        return false;
    }

    struct mntent *ent;
    while ((ent = getmntent(fp)) != NULL)
    {
        if (strcmp(ent->mnt_dir, SDCARD_PREFIX) == 0)
        {
            log_msg("poweroff_next: [DEBUG] is_sdcard_mounted: Found %s mounted\n", SDCARD_PREFIX);
            mounted = true;
            break;
        }
    }

    endmntent(fp);
    log_msg("poweroff_next: [DEBUG] is_sdcard_mounted: Result = %s\n", mounted ? "true" : "false");
    return mounted;
}

static bool unmount_sdcard_with_retries(void)
{
    printf("poweroff_next: [DEBUG] unmount_sdcard_with_retries: Starting\n");
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        log_msg("poweroff_next: [DEBUG] unmount_sdcard_with_retries: Attempt %d/3\n", attempt + 1);
        safe_umount(SDCARD_PREFIX, MNT_FORCE | MNT_DETACH);

        struct timespec wait = {.tv_sec = 0, .tv_nsec = 800000000};
        nanosleep(&wait, NULL);

        if (!is_sdcard_mounted())
        {
            log_msg("poweroff_next: [DEBUG] unmount_sdcard_with_retries: Success on attempt %d\n", attempt + 1);
            return true;
        }

        printf("poweroff_next: [DEBUG] unmount_sdcard_with_retries: Still mounted, killing users\n");
        kill_sdcard_users();
        sync();
    }

    bool result = !is_sdcard_mounted();
    log_msg("poweroff_next: [DEBUG] unmount_sdcard_with_retries: Final result = %s\n", result ? "success" : "failed");
    return result;
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
    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Starting PMIC shutdown sequence\n");
    int fd = open(I2C_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        log_msg("poweroff_next: open(I2C_DEVICE): %s\n", strerror(errno));
        return -1;
    }
    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Opened I2C device\n");

    // Try normal I2C_SLAVE first, then force if busy
    if (ioctl(fd, I2C_SLAVE, AXP2202_ADDR) < 0)
    {
        log_msg("poweroff_next: [DEBUG] ioctl(I2C_SLAVE) failed: %s, trying I2C_SLAVE_FORCE\n", strerror(errno));
        if (ioctl(fd, I2C_SLAVE_FORCE, AXP2202_ADDR) < 0)
        {
            log_msg("poweroff_next: ioctl(I2C_SLAVE_FORCE): %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }
    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Set I2C slave address\n");

    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Writing 0x00 to registers 0x40-0x44\n");
    for (int reg = 0x40; reg <= 0x44; ++reg)
        axp2202_write_reg(fd, (uint8_t)reg, 0x00);

    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Writing 0xFF to registers 0x48-0x4C\n");
    for (int reg = 0x48; reg <= 0x4C; ++reg)
        axp2202_write_reg(fd, (uint8_t)reg, 0xFF);

    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Writing 0x0A to reg 0x22\n");
    axp2202_write_reg(fd, 0x22, 0x0A);
    struct timespec wait = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&wait, NULL);

    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: Writing final poweroff command (0x01 to reg 0x27)\n");
    int ret = axp2202_write_reg(fd, 0x27, 0x01);
    close(fd);
    log_msg("poweroff_next: [DEBUG] execute_axp2202_poweroff: PMIC poweroff command sent, result=%d\n", ret);

    struct timespec latch = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&latch, NULL);

    return ret;
}

static int run_poweroff_protection(void)
{
    log_msg("poweroff_next: Starting power-off protection sequence...\n");

    kill_sdcard_users();
    sync();
    swapoff_all();
    safe_umount("/etc/profile", MNT_FORCE);

    bool unmounted = unmount_sdcard_with_retries();
    if (!unmounted)
        fprintf(stderr, "poweroff_next: SD card remained mounted after retries.\n");

    kill_all_processes();

    // Final sync before PMIC shutdown
    printf("poweroff_next: [DEBUG] Final sync before PMIC shutdown\n");
    sync();
    
    struct timespec pre_pmic_wait = {.tv_sec = 0, .tv_nsec = 500000000};
    nanosleep(&pre_pmic_wait, NULL);

    if (execute_axp2202_poweroff() != 0)
    {
        fprintf(stderr, "poweroff_next: PMIC shutdown sequence failed.\n");
        return -1;
    }

    log_msg("poweroff_next: PMIC software power-off triggered.\n");
    
    // The PMIC should cut power almost immediately, but if we're still running,
    // call the kernel poweroff to ensure shutdown completes
    printf("poweroff_next: [DEBUG] Calling kernel poweroff\n");
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

    log_msg("poweroff_next: Shutting down the system...\n");
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
    
    log_msg("poweroff_next: [DEBUG] main: Starting poweroff_next\n");
    
    // Block SIGTERM and SIGKILL for this process to prevent self-termination
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGTERM);
    sigaddset(&block_set, SIGINT);
    sigaddset(&block_set, SIGHUP);
    sigprocmask(SIG_BLOCK, &block_set, NULL);
    log_msg("poweroff_next: [DEBUG] main: Signals blocked (SIGTERM, SIGINT, SIGHUP)\n");
    
    CFG_init(NULL, NULL);

    bool protection_enabled = CFG_getPowerOffProtection();
    log_msg("poweroff_next: [DEBUG] main: Power-off protection = %s\n", protection_enabled ? "enabled" : "disabled");

    if (protection_enabled)
    {
        printf("poweroff_next: [DEBUG] main: Running protected poweroff sequence\n");
        if (run_poweroff_protection() == 0)
        {
            CFG_quit();
            return 0;
        }

        fprintf(stderr, "poweroff_next: Falling back to standard shutdown.\n");
    }

    printf("poweroff_next: [DEBUG] main: Running standard shutdown\n");
    run_standard_shutdown();
    CFG_quit();
    return 0;
}

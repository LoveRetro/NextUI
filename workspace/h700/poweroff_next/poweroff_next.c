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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "defines.h"
#include "config.h"

// H700 counterpart of tg5040's poweroff_next: the AXP2202 needs an explicit
// software power-off, because the kernel's generic axp20x pm_power_off handler
// writes AXP20X_OFF_CTRL (0x32), which is not the power-off register on this
// chip. Without the sequence below the CPU halts but the rails stay up, which
// reads to the user as "the screen went black but it never turned off" — the
// same limbo the Brick had.
//
// Deliberately *not* a straight copy of the tg5040 tool. See run_flush().

#define AXP_I2C_ADDR 0x34
#define AXP_FALLBACK_DEVICE "/dev/i2c-5"
#define I2C_DEVICES_DIR "/sys/bus/i2c/devices"

// AXP2202 registers (Allwinner BSP names). NOTE the AXP2202 moved the on/off
// control group that the AXP2101 keeps at 0x10 out to 0x27 — writing 0x10 or
// the generic axp20x 0x32 does nothing on this part.
#define AXP_IRQ_EN_FIRST 0x40
#define AXP_IRQ_EN_LAST 0x44
#define AXP_IRQ_STATE_FIRST 0x48
#define AXP_IRQ_STATE_LAST 0x4C
#define AXP_PWROFF_EN 0x22
#define AXP_SOFT_PWROFF 0x27

// PWROFF_EN: bit3 LDO over-current as power-off source, bit1 long-press as
// power-off source, bit2 (die over-temp) cleared, bit0 cleared so a button
// event powers off instead of restarting.
#define AXP_PWROFF_EN_VALUE 0x0A
// SOFT_PWROFF bit0 = power off now. Written as a plain store, not a
// read-modify-write: that also clears bit3 (PWROK pulled low restarts the
// system), which is set on stock H700 firmware and can otherwise bounce the
// SoC back up as the rails collapse.
#define AXP_SOFT_PWROFF_VALUE 0x01

// Deliberately tmpfs, not the card: this path unmounts the card, and the only
// case where the log matters is the one where we failed to power off — in which
// case the device is still running and /tmp is right there over SSH. Keeps the
// shutdown path free of card writes.
#define LOG_PATH "/tmp/poweroff_next.txt"

static FILE *log_fp = NULL;

#ifdef REBOOT_NEXT
#define TOOL_NAME "reboot_next"
#else
#define TOOL_NAME "poweroff_next"
#endif

static void log_msg(const char *format, ...)
{
    va_list args;

    if (log_fp)
    {
        va_start(args, format);
        vfprintf(log_fp, format, args);
        va_end(args);
        fflush(log_fp);
    }

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

static void open_log(void)
{
    log_fp = fopen(LOG_PATH, "w");
    if (log_fp)
        setvbuf(log_fp, NULL, _IONBF, 0); // unbuffered: we may lose power mid-write
}

///////////////////////////////

// /mnt/SDCARD is a symlink (or bind mount) onto /mnt/sdcard on H700, so
// /proc/mounts and /proc/*/fd both name the resolved path. Everything that
// compares against the mount point has to use the resolved one.
static void resolve_sdcard(char *out, size_t len)
{
    char resolved[PATH_MAX];
    if (realpath(SDCARD_PATH, resolved) != NULL)
        snprintf(out, len, "%s", resolved);
    else
        snprintf(out, len, "%s", SDCARD_PATH);
}

static bool is_mounted(const char *path)
{
    FILE *fp = setmntent("/proc/mounts", "r");
    if (!fp)
    {
        log_msg(TOOL_NAME ": fopen(/proc/mounts): %s\n", strerror(errno));
        return false;
    }

    bool mounted = false;
    struct mntent *ent;
    while ((ent = getmntent(fp)) != NULL)
    {
        if (strcmp(ent->mnt_dir, path) == 0)
        {
            mounted = true;
            break;
        }
    }

    endmntent(fp);
    return mounted;
}

static void swapoff_all(void)
{
    FILE *swaps = fopen("/proc/swaps", "r");
    if (!swaps)
        return;

    char line[256];
    if (!fgets(line, sizeof(line), swaps)) // header
    {
        fclose(swaps);
        return;
    }

    while (fgets(line, sizeof(line), swaps))
    {
        char device[128];
        if (sscanf(line, "%127s", device) == 1)
        {
            if (swapoff(device) != 0 && errno != ENOENT && errno != EINVAL)
                log_msg(TOOL_NAME ": swapoff(%s): %s\n", device, strerror(errno));
        }
    }

    fclose(swaps);
}

// Best-effort card flush. Unlike the tg5040 tool this does NOT kill processes.
// On the H700 base OS init respawns the frontend session (`::respawn:` in
// /etc/inittab), so SIGKILLing launch.sh mid-shutdown starts a *new* frontend
// racing the power-off — the exact failure we are here to remove. The PMIC cut
// below is instantaneous and total, so nothing needs to be reaped first; a
// sync plus an unmount attempt is all the protection that buys anything.
static void run_flush(const char *sdcard)
{
    sync();
    swapoff_all();

    if (umount2(sdcard, MNT_DETACH) != 0)
    {
        if (errno != EINVAL && errno != ENOENT)
            log_msg(TOOL_NAME ": umount2(%s): %s\n", sdcard, strerror(errno));
    }

    if (is_mounted(sdcard))
        log_msg(TOOL_NAME ": %s still mounted; relying on sync\n", sdcard);

    sync();
}

///////////////////////////////

#ifndef REBOOT_NEXT

static int axp_open(void)
{
    char device[PATH_MAX];
    snprintf(device, sizeof(device), "%s", AXP_FALLBACK_DEVICE);

    DIR *dir = opendir(I2C_DEVICES_DIR);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            int bus;
            unsigned int addr;
            if (sscanf(entry->d_name, "%d-%x", &bus, &addr) != 2)
                continue;
            if (addr != AXP_I2C_ADDR)
                continue;

            char name_path[PATH_MAX];
            snprintf(name_path, sizeof(name_path), "%s/%s/name", I2C_DEVICES_DIR, entry->d_name);

            FILE *fp = fopen(name_path, "r");
            if (!fp)
                continue;

            char name[64] = {0};
            bool is_axp = fgets(name, sizeof(name), fp) != NULL && strncmp(name, "axp", 3) == 0;
            fclose(fp);

            if (is_axp)
            {
                name[strcspn(name, "\r\n")] = '\0';
                snprintf(device, sizeof(device), "/dev/i2c-%d", bus);
                log_msg(TOOL_NAME ": found %s at %s addr 0x%02x\n", name, device, AXP_I2C_ADDR);
                break;
            }
        }
        closedir(dir);
    }
    else
    {
        log_msg(TOOL_NAME ": opendir(%s): %s\n", I2C_DEVICES_DIR, strerror(errno));
    }

    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        log_msg(TOOL_NAME ": open(%s): %s\n", device, strerror(errno));
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, AXP_I2C_ADDR) < 0 && ioctl(fd, I2C_SLAVE_FORCE, AXP_I2C_ADDR) < 0)
    {
        log_msg(TOOL_NAME ": ioctl(I2C_SLAVE[_FORCE]): %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int axp_write(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = {reg, value};
    if (write(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer))
    {
        log_msg(TOOL_NAME ": write 0x%02x to reg 0x%02x failed: %s\n", value, reg, strerror(errno));
        return -1;
    }
    return 0;
}

// Mask and clear every PMIC interrupt before cutting the rails. A pending,
// unmasked IRQ holds the AXP's IRQ pin low, and >16ms of that powers the PMU
// straight back on. The power-key edge interrupts (PONP/PONN in IRQ_EN1) are
// armed by stock firmware, so the press that asked for power-off is itself the
// most likely thing to undo it. Status registers are write-1-to-clear.
static void axp_quiesce_irqs(int fd)
{
    for (int reg = AXP_IRQ_EN_FIRST; reg <= AXP_IRQ_EN_LAST; ++reg)
        axp_write(fd, (uint8_t)reg, 0x00);

    for (int reg = AXP_IRQ_STATE_FIRST; reg <= AXP_IRQ_STATE_LAST; ++reg)
        axp_write(fd, (uint8_t)reg, 0xFF);
}

static int axp_power_off(bool dry_run)
{
    int fd = axp_open();
    if (fd < 0)
        return -1;

    if (dry_run)
    {
        log_msg(TOOL_NAME ": dry run, would write 0x%02x<-0x%02x then 0x%02x<-0x%02x"
                          " after masking 0x%02x-0x%02x and clearing 0x%02x-0x%02x\n",
                AXP_PWROFF_EN, AXP_PWROFF_EN_VALUE, AXP_SOFT_PWROFF, AXP_SOFT_PWROFF_VALUE,
                AXP_IRQ_EN_FIRST, AXP_IRQ_EN_LAST, AXP_IRQ_STATE_FIRST, AXP_IRQ_STATE_LAST);
        close(fd);
        return 0;
    }

    axp_quiesce_irqs(fd);

    axp_write(fd, AXP_PWROFF_EN, AXP_PWROFF_EN_VALUE);

    struct timespec settle = {.tv_sec = 0, .tv_nsec = 50000000}; // 50ms
    nanosleep(&settle, NULL);

    int ret = axp_write(fd, AXP_SOFT_PWROFF, AXP_SOFT_PWROFF_VALUE);
    close(fd);

    // The rails should drop within milliseconds; give them a moment before the
    // kernel fallback so we do not race the PMIC.
    struct timespec latch = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&latch, NULL);

    return ret;
}

#endif // !REBOOT_NEXT

///////////////////////////////

static void finalize(void)
{
    sync();

#ifdef REBOOT_NEXT
    int cmd = LINUX_REBOOT_CMD_RESTART;
#else
    int cmd = LINUX_REBOOT_CMD_POWER_OFF;
#endif

    if (syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, cmd, NULL) != 0)
        log_msg(TOOL_NAME ": reboot syscall failed: %s\n", strerror(errno));

#ifdef REBOOT_NEXT
    execlp("busybox", "busybox", "reboot", "-f", NULL);
    reboot(RB_AUTOBOOT);
#else
    execlp("busybox", "busybox", "poweroff", "-f", NULL);
    reboot(RB_POWER_OFF);
#endif

    log_msg(TOOL_NAME ": every shutdown method failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
    // --dry-run reports what would happen (PMIC bus detection, resolved card
    // path, protection setting) and touches nothing. Safe to run on a live
    // device; that is the point of it.
    bool dry_run = false;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = true;
    }

    open_log();

    // init's shutdown sweep must not take us out mid-sequence.
    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGTERM);
    sigaddset(&block_set, SIGINT);
    sigaddset(&block_set, SIGHUP);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    CFG_init(NULL, NULL);

    char sdcard[PATH_MAX];
    resolve_sdcard(sdcard, sizeof(sdcard));

    bool flush = CFG_getPowerOffProtection();
    log_msg(TOOL_NAME ": sdcard=%s mounted=%s flush=%s dry_run=%s\n",
            sdcard, is_mounted(sdcard) ? "yes" : "no",
            flush ? "yes" : "no", dry_run ? "yes" : "no");

#ifndef REBOOT_NEXT
    // Reboot deliberately skips the PMIC: a restart is the SoC's job, and
    // AXP2202 soft-reset is a different bit we have no need to poke.
    if (dry_run)
    {
        int ret = axp_power_off(true);
        CFG_quit();
        return ret == 0 ? 0 : EXIT_FAILURE;
    }
#else
    if (dry_run)
    {
        CFG_quit();
        return 0;
    }
#endif

    if (flush)
        run_flush(sdcard);
    else
        sync();

#ifndef REBOOT_NEXT
    if (axp_power_off(false) != 0)
        log_msg(TOOL_NAME ": PMIC power-off failed; falling back to the kernel\n");
#endif

    finalize();

    // finalize() only returns when every shutdown path failed. Exit non-zero so
    // the `poweroff_next || poweroff` fallback in launch.sh actually fires.
    log_msg(TOOL_NAME ": still running after finalize; deferring to the caller\n");
    CFG_quit();
    return EXIT_FAILURE;
}

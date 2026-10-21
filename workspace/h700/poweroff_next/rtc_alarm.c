#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/rtc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_RTC_DEVICE "/dev/rtc0"
#define DEFAULT_RTC_WAKEUP "/sys/class/rtc/rtc0/device/power/wakeup"
#define DEFAULT_ALARM_RESOLUTION_SECONDS 60

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s enable|arm <seconds>|clear\n", program);
}

static int read_wakeup_status(const char *path, char *status, size_t status_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "rtc_alarm: failed to open wake source %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    ssize_t length = read(fd, status, status_size - 1);
    int read_errno = errno;
    if (close(fd) < 0 && length >= 0) {
        fprintf(stderr, "rtc_alarm: failed to close wake source %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (length < 0) {
        fprintf(stderr, "rtc_alarm: failed to read wake source %s: %s\n",
                path, strerror(read_errno));
        return -1;
    }
    if (length == 0) {
        fprintf(stderr, "rtc_alarm: wake source %s returned an empty status\n", path);
        return -1;
    }

    status[length] = '\0';
    status[strcspn(status, "\r\n")] = '\0';
    return 0;
}

static int enable_wakeup(const char *path)
{
    char status[32];
    if (read_wakeup_status(path, status, sizeof(status)) < 0)
        return -1;
    if (strcmp(status, "enabled") == 0) {
        fprintf(stderr, "rtc_alarm: wake source already enabled (%s)\n", path);
        return 0;
    }
    if (strcmp(status, "disabled") != 0) {
        fprintf(stderr, "rtc_alarm: invalid wake source status '%s' from %s\n",
                status, path);
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "rtc_alarm: failed to open wake source %s for writing: %s\n",
                path, strerror(errno));
        return -1;
    }

    static const char enabled[] = "enabled\n";
    ssize_t written = write(fd, enabled, sizeof(enabled) - 1);
    int write_errno = errno;
    if (close(fd) < 0 && written >= 0) {
        fprintf(stderr, "rtc_alarm: failed to close wake source %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (written < 0) {
        fprintf(stderr, "rtc_alarm: failed to enable wake source %s: %s\n",
                path, strerror(write_errno));
        return -1;
    }
    if (written != (ssize_t)(sizeof(enabled) - 1)) {
        fprintf(stderr, "rtc_alarm: short write enabling wake source %s (%zd of %zu)\n",
                path, written, sizeof(enabled) - 1);
        return -1;
    }

    if (read_wakeup_status(path, status, sizeof(status)) < 0)
        return -1;
    if (strcmp(status, "enabled") != 0) {
        fprintf(stderr, "rtc_alarm: wake source %s remained '%s' after enable\n",
                path, status);
        return -1;
    }

    fprintf(stderr, "rtc_alarm: wake source enabled (%s)\n", path);
    return 0;
}

static int rtc_time_to_epoch(const struct rtc_time *rtc_time, int64_t *epoch)
{
    struct tm tm = {
        .tm_sec = rtc_time->tm_sec,
        .tm_min = rtc_time->tm_min,
        .tm_hour = rtc_time->tm_hour,
        .tm_mday = rtc_time->tm_mday,
        .tm_mon = rtc_time->tm_mon,
        .tm_year = rtc_time->tm_year,
        .tm_isdst = 0,
    };

    errno = 0;
    time_t value = timegm(&tm);
    if (value == (time_t)-1 && errno != 0) {
        fprintf(stderr, "rtc_alarm: failed to convert RTC time: %s\n", strerror(errno));
        return -1;
    }

    *epoch = (int64_t)value;
    return 0;
}

static int epoch_to_rtc_time(int64_t epoch, struct rtc_time *rtc_time)
{
    time_t value = (time_t)epoch;
    struct tm tm;

    if ((int64_t)value != epoch || !gmtime_r(&value, &tm)) {
        fprintf(stderr, "rtc_alarm: alarm time is outside the supported range\n");
        return -1;
    }

    rtc_time->tm_sec = tm.tm_sec;
    rtc_time->tm_min = tm.tm_min;
    rtc_time->tm_hour = tm.tm_hour;
    rtc_time->tm_mday = tm.tm_mday;
    rtc_time->tm_mon = tm.tm_mon;
    rtc_time->tm_year = tm.tm_year;
    rtc_time->tm_wday = tm.tm_wday;
    rtc_time->tm_yday = tm.tm_yday;
    rtc_time->tm_isdst = 0;
    return 0;
}

static int read_alarm(int fd, struct rtc_wkalrm *alarm, int64_t *epoch)
{
    memset(alarm, 0, sizeof(*alarm));
    if (ioctl(fd, RTC_WKALM_RD, alarm) < 0) {
        fprintf(stderr, "rtc_alarm: RTC_WKALM_RD failed: %s\n", strerror(errno));
        return -1;
    }

    if (rtc_time_to_epoch(&alarm->time, epoch) < 0)
        return -1;

    return 0;
}

static int clear_alarm(int fd)
{
    struct rtc_wkalrm alarm;
    int64_t alarm_epoch;

    if (read_alarm(fd, &alarm, &alarm_epoch) < 0)
        return -1;

    unsigned int was_enabled = alarm.enabled;
    unsigned int was_pending = alarm.pending;
    if (ioctl(fd, RTC_AIE_OFF, 0) < 0) {
        fprintf(stderr,
                "rtc_alarm: RTC_AIE_OFF failed for alarm %" PRId64
                " (enabled=%u pending=%u): %s\n",
                alarm_epoch, was_enabled, was_pending, strerror(errno));
        return -1;
    }

    if (read_alarm(fd, &alarm, &alarm_epoch) < 0)
        return -1;
    if (alarm.enabled || alarm.pending) {
        fprintf(stderr,
                "rtc_alarm: alarm interrupt remained active after RTC_AIE_OFF"
                " (%" PRId64 ", enabled=%u pending=%u)\n",
                alarm_epoch, alarm.enabled, alarm.pending);
        return -1;
    }

    fprintf(stderr,
            "rtc_alarm: alarm interrupt cleared (was enabled=%u pending=%u)\n",
            was_enabled, was_pending);
    return 0;
}

static int parse_delay(const char *seconds_arg, uint64_t *delay)
{
    char *end = NULL;
    errno = 0;
    uint64_t value = strtoull(seconds_arg, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "rtc_alarm: invalid delay '%s'\n", seconds_arg);
        return -1;
    }

    *delay = value;
    return 0;
}

static int alarm_resolution(uint64_t *resolution)
{
    const char *resolution_arg = getenv("NEXTUI_RTC_RESOLUTION_SECONDS");
    if (!resolution_arg || !*resolution_arg) {
        *resolution = DEFAULT_ALARM_RESOLUTION_SECONDS;
        return 0;
    }

    char *end = NULL;
    errno = 0;
    uint64_t value = strtoull(resolution_arg, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "rtc_alarm: invalid alarm resolution '%s'\n", resolution_arg);
        return -1;
    }

    *resolution = value;
    return 0;
}

static int arm_alarm(int fd, uint64_t delay, uint64_t resolution)
{
    /* Explicitly disable the previous alarm interrupt before every arm. This
     * follows the reliable clear-then-set sequence while avoiding wakealarm
     * sysfs EBUSY handling. */
    if (clear_alarm(fd) < 0) {
        fprintf(stderr, "rtc_alarm: refusing to arm without clearing the prior alarm interrupt\n");
        return -1;
    }

    struct rtc_time now_time;
    if (ioctl(fd, RTC_RD_TIME, &now_time) < 0) {
        fprintf(stderr, "rtc_alarm: RTC_RD_TIME failed: %s\n", strerror(errno));
        return -1;
    }

    int64_t now;
    if (rtc_time_to_epoch(&now_time, &now) < 0)
        return -1;
    if (now < 0 || resolution > (uint64_t)(INT64_MAX - now) ||
        delay > (uint64_t)(INT64_MAX - now - (int64_t)(resolution - 1))) {
        fprintf(stderr, "rtc_alarm: requested deadline overflows supported time\n");
        return -1;
    }

    int64_t minimum_deadline = now + (int64_t)delay;
    int64_t deadline = ((minimum_deadline + (int64_t)resolution - 1) /
                        (int64_t)resolution) * (int64_t)resolution;

    struct rtc_wkalrm alarm;
    memset(&alarm, 0, sizeof(alarm));
    alarm.enabled = 1;
    if (epoch_to_rtc_time(deadline, &alarm.time) < 0)
        return -1;

    /* RTC_WKALM_SET arms the replacement after RTC_AIE_OFF cleared any stale
     * alarm flag or interrupt state. */
    if (ioctl(fd, RTC_WKALM_SET, &alarm) < 0) {
        fprintf(stderr, "rtc_alarm: RTC_WKALM_SET failed for %" PRId64 ": %s\n",
                deadline, strerror(errno));
        return -1;
    }

    int64_t armed_epoch;
    if (read_alarm(fd, &alarm, &armed_epoch) < 0)
        return -1;
    if (!alarm.enabled) {
        fprintf(stderr, "rtc_alarm: alarm readback is disabled\n");
        return -1;
    }
    if (armed_epoch < minimum_deadline || armed_epoch > deadline + (int64_t)resolution) {
        fprintf(stderr,
                "rtc_alarm: alarm readback mismatch (minimum=%" PRId64
                " requested=%" PRId64 " actual=%" PRId64 ")\n",
                minimum_deadline, deadline, armed_epoch);
        return -1;
    }

    fprintf(stderr,
            "rtc_alarm: armed after %" PRIu64 " seconds (RTC %" PRId64
            " -> %" PRId64 ", resolution=%" PRIu64 "s)\n",
            delay, now, armed_epoch, resolution);
    printf("%" PRId64 "\n", armed_epoch);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "arm") == 0 && argc != 3) ||
        (strcmp(argv[1], "clear") == 0 && argc != 2) ||
        (strcmp(argv[1], "enable") == 0 && argc != 2)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *rtc_wakeup = getenv("NEXTUI_RTC_WAKEUP");
    if (!rtc_wakeup || !*rtc_wakeup)
        rtc_wakeup = DEFAULT_RTC_WAKEUP;

    if (strcmp(argv[1], "enable") == 0)
        return enable_wakeup(rtc_wakeup) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    if (strcmp(argv[1], "arm") != 0 && strcmp(argv[1], "clear") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "arm") == 0 && enable_wakeup(rtc_wakeup) < 0) {
        fprintf(stderr, "rtc_alarm: refusing to arm without a verified wake source\n");
        return EXIT_FAILURE;
    }

    const char *rtc_device = getenv("NEXTUI_RTC_DEVICE");
    if (!rtc_device || !*rtc_device)
        rtc_device = DEFAULT_RTC_DEVICE;

    int fd = open(rtc_device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "rtc_alarm: failed to open %s: %s\n", rtc_device, strerror(errno));
        return EXIT_FAILURE;
    }

    int result;
    if (strcmp(argv[1], "arm") == 0) {
        uint64_t delay;
        uint64_t resolution;
        result = parse_delay(argv[2], &delay);
        if (result == 0)
            result = alarm_resolution(&resolution);
        if (result == 0)
            result = arm_alarm(fd, delay, resolution);
    } else if (strcmp(argv[1], "clear") == 0) {
        result = clear_alarm(fd);
    } else {
        result = clear_alarm(fd);
    }

    if (close(fd) < 0 && result == 0) {
        fprintf(stderr, "rtc_alarm: failed to close %s: %s\n", rtc_device, strerror(errno));
        result = -1;
    }
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

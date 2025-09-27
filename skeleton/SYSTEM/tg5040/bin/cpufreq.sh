#!/bin/sh
#
# cpufreq.sh - CPU governor and frequency control for Allwinner A133p (Tina Linux)
#

CPU_BASE="/sys/devices/system/cpu"
AVAIL_GOVS="$CPU_BASE/cpu0/cpufreq/scaling_available_governors"
AVAIL_FREQS="$CPU_BASE/cpu0/cpufreq/scaling_available_frequencies"

# Default fallback governor
DEFAULT_GOV="schedutil"

# Helpers
set_for_all_cpus() {
    local file=$1
    local value=$2
    for cpu in $CPU_BASE/cpu[0-9]*; do
        if [ -w "$cpu/cpufreq/$file" ]; then
            echo "$value" > "$cpu/cpufreq/$file" 2>/dev/null
        fi
    done
}

get_max_freq() {
    cat $AVAIL_FREQS 2>/dev/null | awk '{print $NF}'
}
get_min_freq() {
    cat $AVAIL_FREQS 2>/dev/null | awk '{print $1}'
}

has_governor() {
    grep -qw "$1" "$AVAIL_GOVS" 2>/dev/null
}

reset_cpufreq() {
    echo "Resetting CPU frequency policy to defaults"

    # Ensure all cores are online
    for cpu in $CPU_BASE/cpu[0-9]*; do
        if [ -w "$cpu/online" ]; then
            echo 1 > "$cpu/online" 2>/dev/null
        fi
    done

    # Reset frequency limits
    for cpu in $CPU_BASE/cpu[0-9]*; do
        min=$(cat $cpu/cpufreq/cpuinfo_min_freq 2>/dev/null)
        max=$(cat $cpu/cpufreq/cpuinfo_max_freq 2>/dev/null)
        [ -w "$cpu/cpufreq/scaling_min_freq" ] && echo "$min" > "$cpu/cpufreq/scaling_min_freq"
        [ -w "$cpu/cpufreq/scaling_max_freq" ] && echo "$max" > "$cpu/cpufreq/scaling_max_freq"
    done

    # Reset governor
    if has_governor "$DEFAULT_GOV"; then
        echo "Governor reset to $DEFAULT_GOV"
        set_for_all_cpus scaling_governor "$DEFAULT_GOV"
    else
        echo "Governor $DEFAULT_GOV not available, falling back to performance"
        set_for_all_cpus scaling_governor "performance"
    fi
}

detect_profile() {
    gov=$(cat $CPU_BASE/cpu0/cpufreq/scaling_governor 2>/dev/null)
    cur=$(cat $CPU_BASE/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
    min=$(cat $CPU_BASE/cpu0/cpufreq/cpuinfo_min_freq 2>/dev/null)
    max=$(cat $CPU_BASE/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null)

    case "$gov" in
        performance) echo "performance" ;;
        powersave)   echo "powersave" ;;
        ondemand)    echo "ondemand" ;;
        schedutil)   echo "schedutil" ;;
        interactive) echo "interactive" ;;
        conservative)echo "conservative" ;;
        userspace)
            if [ "$cur" = "$max" ]; then
                echo "userspace (max freq)"
            elif [ "$cur" = "$min" ]; then
                echo "userspace (min freq)"
            else
                echo "userspace (custom freq: $cur)"
            fi
            ;;
        *) echo "unknown governor: $gov" ;;
    esac
}

show_status() {
    echo "=== CPU Frequency/Governor Status ==="
    echo "Available governors: $(cat $AVAIL_GOVS 2>/dev/null)"
    echo "Available freqs:     $(cat $AVAIL_FREQS 2>/dev/null)"
    echo "Active profile:      $(detect_profile)"
    for cpu in $CPU_BASE/cpu[0-9]*; do
        online="1"
        [ -r "$cpu/online" ] && online=$(cat "$cpu/online")
        gov=$(cat $cpu/cpufreq/scaling_governor 2>/dev/null)
        cur=$(cat $cpu/cpufreq/scaling_cur_freq 2>/dev/null)
        smin=$(cat $cpu/cpufreq/scaling_min_freq 2>/dev/null)
        smax=$(cat $cpu/cpufreq/scaling_max_freq 2>/dev/null)
        min=$(cat $cpu/cpufreq/cpuinfo_min_freq 2>/dev/null)
        max=$(cat $cpu/cpufreq/cpuinfo_max_freq 2>/dev/null)
        echo "$(basename $cpu): online=$online governor=$gov cur=${cur}"
        echo "   scaling_min=${smin} scaling_max=${smax} (hardware min=${min} max=${max})"
    done
}

list_profiles() {
    echo "Supported profiles on this system:"
    for gov in $(cat $AVAIL_GOVS 2>/dev/null); do
        echo "  $gov"
    done
    echo "Special modes:"
    echo "  reset   (restore defaults: $DEFAULT_GOV)"
    echo "  status  (show current info)"
    echo "  list    (list supported governors)"
    echo "  freq    (set specific frequency, requires -f <Hz>, userspace governor)"
}

usage() {
    echo "Usage: $0 -p <profile> [-f <freq>]"
    echo "Profiles:"
    echo "   performance   - max speed"
    echo "   powersave     - min speed"
    echo "   ondemand      - ondemand governor"
    echo "   schedutil     - schedutil governor (if supported)"
    echo "   interactive   - interactive governor (if supported)"
    echo "   conservative  - conservative governor (if supported)"
    echo "   reset         - restore defaults (all cores online, default min/max, default governor)"
    echo "   status        - show current governor, freq, active profile, and CPU online status"
    echo "   list          - list supported governors"
    echo "   freq          - set exact frequency (requires -f <Hz>, userspace governor)"
    exit 1
}

PROFILE=""
FREQ=""

while getopts "p:f:" opt; do
    case $opt in
        p) PROFILE="$OPTARG" ;;
        f) FREQ="$OPTARG" ;;
        *) usage ;;
    esac
done

[ -z "$PROFILE" ] && usage

case "$PROFILE" in
    performance)
        if has_governor performance; then
            set_for_all_cpus scaling_governor performance
            set_for_all_cpus scaling_setspeed "$(get_max_freq)"
        fi
        ;;
    powersave)
        if has_governor powersave; then
            set_for_all_cpus scaling_governor powersave
            set_for_all_cpus scaling_setspeed "$(get_min_freq)"
        fi
        ;;
    ondemand|schedutil|interactive|conservative)
        if has_governor "$PROFILE"; then
            set_for_all_cpus scaling_governor "$PROFILE"
        else
            echo "Governor $PROFILE not supported, falling back to $DEFAULT_GOV"
            reset_cpufreq
        fi
        ;;
    reset)
        reset_cpufreq
        ;;
    status)
        show_status
        ;;
    list)
        list_profiles
        ;;
    freq)
        if [ -z "$FREQ" ]; then
            echo "Error: freq profile requires -f <Hz>"
            exit 1
        fi
        if has_governor userspace; then
            echo "Setting userspace governor at $FREQ Hz"
            set_for_all_cpus scaling_governor userspace
            set_for_all_cpus scaling_setspeed "$FREQ"
        else
            echo "userspace governor not supported!"
            exit 1
        fi
        ;;
    *)
        echo "Unknown profile: $PROFILE"
        usage
        ;;
esac

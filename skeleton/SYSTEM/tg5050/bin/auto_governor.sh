#!/bin/sh
# auto_governor.sh - ondemand governor, min freq to one step below max
# All cpu*/cpufreq entries point at this same shared policy0 path.
CPU_PATH=/sys/devices/system/cpu/cpufreq/policy0
[ -f "$CPU_PATH/scaling_available_frequencies" ] || exit 0
FREQS=$(cat "$CPU_PATH/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
MIN_FREQ=$(echo "$FREQS" | head -1)
SECOND_MAX=$(echo "$FREQS" | tail -2 | head -1)
echo ondemand > "$CPU_PATH/scaling_governor" 2>/dev/null || true
echo "$MIN_FREQ" > "$CPU_PATH/scaling_min_freq"
echo "$SECOND_MAX" > "$CPU_PATH/scaling_max_freq"

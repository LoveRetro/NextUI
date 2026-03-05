#!/bin/sh
# powersave_governor.sh - conservative governor, min freq to midpoint max
for CPU_PATH in /sys/devices/system/cpu/cpu*/cpufreq; do
    [ -f "$CPU_PATH/scaling_available_frequencies" ] || continue
    FREQS=$(cat "$CPU_PATH/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
    COUNT=$(echo "$FREQS" | wc -l)
    MID=$(( (COUNT + 1) / 2 ))
    MIN_FREQ=$(echo "$FREQS" | head -1)
    MID_FREQ=$(echo "$FREQS" | sed -n "${MID}p")
    echo conservative > "$CPU_PATH/scaling_governor" 2>/dev/null || true
    echo "$MIN_FREQ" > "$CPU_PATH/scaling_min_freq"
    echo "$MID_FREQ" > "$CPU_PATH/scaling_max_freq"
done

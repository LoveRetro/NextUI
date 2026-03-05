#!/bin/sh
# performance_governor.sh - schedutil governor, min freq to max freq
for CPU_PATH in /sys/devices/system/cpu/cpu*/cpufreq; do
    [ -f "$CPU_PATH/scaling_available_frequencies" ] || continue
    FREQS=$(cat "$CPU_PATH/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
    MIN_FREQ=$(echo "$FREQS" | head -1)
    MAX_FREQ=$(echo "$FREQS" | tail -1)
    echo schedutil > "$CPU_PATH/scaling_governor" 2>/dev/null || true
    echo "$MIN_FREQ" > "$CPU_PATH/scaling_min_freq"
    echo "$MAX_FREQ" > "$CPU_PATH/scaling_max_freq"
done

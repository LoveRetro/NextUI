#!/bin/sh
# performance_governor.sh - schedutil governor, min freq to max freq
# policy0 - little Cores (408-1416 MHz on TG5050)
# policy4 - BIG Cores (408-2160 MHz on TG5050)

set_policy() {
	local policy_path="$1"
	[ -f "$policy_path/scaling_available_frequencies" ] || return 0
	FREQS=$(cat "$policy_path/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
	MIN_FREQ=$(echo "$FREQS" | head -1)
	MAX_FREQ=$(echo "$FREQS" | tail -1)
	echo schedutil > "$policy_path/scaling_governor" 2>/dev/null || true
	echo "$MIN_FREQ" > "$policy_path/scaling_min_freq"
	echo "$MAX_FREQ" > "$policy_path/scaling_max_freq"
}

set_policy /sys/devices/system/cpu/cpufreq/policy0
set_policy /sys/devices/system/cpu/cpufreq/policy4

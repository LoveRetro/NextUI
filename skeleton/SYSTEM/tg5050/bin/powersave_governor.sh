#!/bin/sh
# powersave_governor.sh - conservative governor, min freq to midpoint max
# policy0 - little Cores (408-1032 MHz on TG5050)
# policy4 - BIG Cores (408-1488 MHz on TG5050)

set_policy() {
	local policy_path="$1"
	[ -f "$policy_path/scaling_available_frequencies" ] || return 0
	FREQS=$(cat "$policy_path/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
	COUNT=$(echo "$FREQS" | wc -l)
	MID=$(( (COUNT + 1) / 2 ))
	MIN_FREQ=$(echo "$FREQS" | head -1)
	MID_FREQ=$(echo "$FREQS" | sed -n "${MID}p")
	echo conservative > "$policy_path/scaling_governor" 2>/dev/null || true
	echo "$MIN_FREQ" > "$policy_path/scaling_min_freq"
	echo "$MID_FREQ" > "$policy_path/scaling_max_freq"
}

set_policy /sys/devices/system/cpu/cpufreq/policy0
set_policy /sys/devices/system/cpu/cpufreq/policy4

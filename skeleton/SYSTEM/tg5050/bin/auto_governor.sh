#!/bin/sh
# auto_governor.sh - ondemand governor, min freq to one step below max
# policy0 - little Cores (408-1320 MHz on TG5050)
# policy4 - BIG Cores (408-2088 MHz on TG5050)

set_policy() {
	local policy_path="$1"
	[ -f "$policy_path/scaling_available_frequencies" ] || return 0
	FREQS=$(cat "$policy_path/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
	MIN_FREQ=$(echo "$FREQS" | head -1)
	SECOND_MAX=$(echo "$FREQS" | tail -2 | head -1)
	echo ondemand > "$policy_path/scaling_governor" 2>/dev/null || true
	echo "$MIN_FREQ" > "$policy_path/scaling_min_freq"
	echo "$SECOND_MAX" > "$policy_path/scaling_max_freq"
}

set_policy /sys/devices/system/cpu/cpufreq/policy0
set_policy /sys/devices/system/cpu/cpufreq/policy4

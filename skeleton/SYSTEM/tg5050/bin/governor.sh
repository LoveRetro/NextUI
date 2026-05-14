#!/bin/sh
# governor.sh - CPU governor controller for TG5050
# Usage: governor.sh <mode>
# Modes: auto, performance, powersave

MODE="$1"
[ -z "$MODE" ] && MODE="auto"

set_policy() {
	local policy_path="$1"
	local governor="$2"
	local max_type="$3"  # "second_max", "max", or "mid"
	
	[ -f "$policy_path/scaling_available_frequencies" ] || return 0
	FREQS=$(cat "$policy_path/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
	MIN_FREQ=$(echo "$FREQS" | head -1)
	
	case "$max_type" in
		second_max)
			MAX_FREQ=$(echo "$FREQS" | tail -2 | head -1)
			;;
		max)
			MAX_FREQ=$(echo "$FREQS" | tail -1)
			;;
		mid)
			COUNT=$(echo "$FREQS" | wc -l)
			MID=$(( (COUNT + 1) / 2 ))
			MAX_FREQ=$(echo "$FREQS" | sed -n "${MID}p")
			;;
		*)
			MAX_FREQ=$(echo "$FREQS" | tail -1)
			;;
	esac
	
	echo "$governor" > "$policy_path/scaling_governor" 2>/dev/null || true
	echo "$MIN_FREQ" > "$policy_path/scaling_min_freq"
	echo "$MAX_FREQ" > "$policy_path/scaling_max_freq"
}

apply_mode() {
	local mode="$1"
	local policy="$2"
	
	case "$mode" in
		auto)
			# ondemand governor, min freq to one step below max
			# policy0 - little Cores (408-1320 MHz on TG5050)
			# policy4 - BIG Cores (408-2088 MHz on TG5050)
			set_policy "$policy" "ondemand" "second_max"
			;;
		performance)
			# schedutil governor, min freq to max freq
			# policy0 - little Cores (408-1416 MHz on TG5050)
			# policy4 - BIG Cores (408-2160 MHz on TG5050)
			set_policy "$policy" "schedutil" "max"
			;;
		powersave)
			# conservative governor, min freq to midpoint max
			# policy0 - little Cores (408-1032 MHz on TG5050)
			# policy4 - BIG Cores (408-1488 MHz on TG5050)
			set_policy "$policy" "conservative" "mid"
			;;
		*)
			echo "governor.sh: unknown mode '$mode'" >&2
			echo "  Valid modes: auto, performance, powersave" >&2
			return 1
			;;
	esac
}

apply_mode "$MODE" /sys/devices/system/cpu/cpufreq/policy0
apply_mode "$MODE" /sys/devices/system/cpu/cpufreq/policy4

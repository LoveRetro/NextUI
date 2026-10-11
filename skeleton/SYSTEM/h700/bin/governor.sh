#!/bin/sh
# governor.sh - CPU/GPU governor controller for H700
# Usage: governor.sh <mode>
# Modes: auto, performance, powersave

MODE="$1"
[ -z "$MODE" ] && MODE="auto"

set_policy() {
	local policy_path="$1"
	local governor="$2"
	local max_type="$3"  # "max" or "mid"
	
	[ -f "$policy_path/scaling_available_frequencies" ] || return 0
	FREQS=$(cat "$policy_path/scaling_available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
	MIN_FREQ=$(echo "$FREQS" | head -1)
	
	case "$max_type" in
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

# The simple_ondemand GPU governor doesn't perform well under vsync.
# Pin to max GPU speed. It costs about 0.5% more battery usage (when screen is at
# full power), but the performance gain is worth it in my opinion. The power gating
# will reduce GPU power when it's not busy anyway: so it's a race-to-idle. Or at
# least that's my logic.
set_gpu_floor() {
	local floor_type="$1"  # "max" or "min"

	for devfreq in /sys/class/devfreq/*gpu*; do
		[ -f "$devfreq/available_frequencies" ] || continue
		[ -w "$devfreq/min_freq" ] || continue

		GPU_FREQS=$(cat "$devfreq/available_frequencies" | tr ' ' '\n' | grep -v '^$' | sort -n)
		case "$floor_type" in
			max)
				GPU_FREQ=$(echo "$GPU_FREQS" | tail -1)
				;;
			*)
				GPU_FREQ=$(echo "$GPU_FREQS" | head -1)
				;;
		esac

		[ -n "$GPU_FREQ" ] || continue
		echo "$GPU_FREQ" > "$devfreq/min_freq" 2>/dev/null || true
	done
}

case "$MODE" in
	auto)
		# H700's advertised 1.5 GHz ceiling is in-spec, not an overclock. Let
		# schedutil use the full hardware frequency range when load requires it.
		set_policy /sys/devices/system/cpu/cpufreq/policy0 "schedutil" "max"
		set_policy /sys/devices/system/cpu/cpu0/cpufreq "schedutil" "max"
		set_gpu_floor "max"
		;;
	performance)
		set_policy /sys/devices/system/cpu/cpufreq/policy0 "performance" "max"
		set_policy /sys/devices/system/cpu/cpu0/cpufreq "performance" "max"
		set_gpu_floor "max"
		;;
	powersave)
		set_policy /sys/devices/system/cpu/cpufreq/policy0 "conservative" "mid"
		set_policy /sys/devices/system/cpu/cpu0/cpufreq "conservative" "mid"
		set_gpu_floor "min"
		;;
	*)
		echo "governor.sh: unknown mode '$MODE'" >&2
		echo "  Valid modes: auto, performance, powersave" >&2
		exit 1
		;;
esac

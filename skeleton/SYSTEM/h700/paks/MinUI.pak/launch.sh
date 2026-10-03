#!/bin/sh
# MinUI.pak for Anbernic RG XX / Allwinner H700

export PLATFORM="h700"
# Load beside the launcher: /mnt/SDCARD may not be available until mounted below.
. "$(dirname "$0")/../../bin/h700-init.sh"

h700_mount_sdcard

export SDCARD_PATH="$COMPAT_SDCARD_PATH"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export HOOKS_PATH="$USERDATA_PATH/.hooks"
export HOME="$USERDATA_PATH"
LAUNCH_LOG="$LOGS_PATH/launch.txt"

# H700 needs the bundled runtime even for the early poweroff/reboot checks.
# Prefer bundled tools (including curl, absent from vanilla stock OS) and
# libraries, especially the patched SDL2 used by this port.
export PATH="$SYSTEM_PATH/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
export LD_LIBRARY_PATH="$SYSTEM_PATH/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
h700_setup_runtime

#######################################

if [ -f "/tmp/poweroff" ]; then
	poweroff_next || poweroff
	exit 0
fi
if [ -f "/tmp/reboot" ]; then
	reboot_next || reboot
	exit 0
fi

#######################################

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$HOOKS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"
echo "launch: starting $(date)" > "$LAUNCH_LOG"

. "$SYSTEM_PATH/bin/h700-device.sh"
h700_export_device
if [ "$DEVICE" = "rg28xx" ]; then
	export SDL_ROTATION="${SDL_ROTATION:-1}"
fi
export IS_NEXT="yes"
log_runtime_state

#######################################

h700_prepare_stock_os

# clear shadercache unconditionally, until it properly invalidates itself
rm -rf "$SDCARD_PATH/.shadercache"

h700_init_leds

#######################################

sh "$SYSTEM_PATH/bin/governor.sh" "auto"

keymon.elf > "$LOGS_PATH/keymon.txt" 2>&1 &
batmon.elf > "$LOGS_PATH/batmon.txt" 2>&1 &

# start fresh, will be populated on the next connect
rm -f "$USERDATA_PATH/.asoundrc"
ensure_system_dbus || echo "launch: system D-Bus unavailable; audio device monitoring may be disabled" >> "$LAUNCH_LOG"
audiomon.elf > "$LOGS_PATH/audiomon.txt" 2>&1 &

# BT handling
# on by default, disable based on systemval setting
bluetoothon=$(nextval.elf bluetooth | sed -n 's/.*"bluetooth": \([0-9]*\).*/\1/p')
if [ "$bluetoothon" = "0" ]; then
	"$SYSTEM_PATH/etc/bluetooth/bt_init.sh" stop > /dev/null 2>&1 &
else
	"$SYSTEM_PATH/etc/bluetooth/bt_init.sh" start > /dev/null 2>&1 &
fi

# wifi handling
# on by default, disable based on systemval setting
wifion=$(nextval.elf wifi | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
if [ "$wifion" = "0" ]; then
	"$SYSTEM_PATH/etc/wifi/wifi_init.sh" stop > /dev/null 2>&1 &
else
	"$SYSTEM_PATH/etc/wifi/wifi_init.sh" start > /dev/null 2>&1 &
fi

#######################################

AUTO_PATH="$USERDATA_PATH/auto.sh"
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

# Composable boot hooks (run after auto.sh for backward compatibility)
"$SYSTEM_PATH/bin/run_hooks.sh" boot.d

cd "$(dirname "$0")"

#######################################
# Hook system

parse_hook_cmd() {
	HOOK_CMD="$1"
	HOOK_EMU_PATH=$(echo "$HOOK_CMD" | sed "s/^'\\([^']*\\)'.*/\\1/")
	_remainder=$(echo "$HOOK_CMD" | sed "s/^'[^']*'//")
	if echo "$_remainder" | grep -q "'"; then
		HOOK_TYPE="rom"
		HOOK_ROM_PATH=$(echo "$_remainder" | sed "s/.*'\\([^']*\\)'.*/\\1/")
	else
		HOOK_TYPE="pak"
		HOOK_ROM_PATH=""
	fi
	[ -f /tmp/last.txt ] && HOOK_LAST=$(cat /tmp/last.txt) || HOOK_LAST=""
	export HOOK_CMD HOOK_EMU_PATH HOOK_TYPE HOOK_ROM_PATH HOOK_LAST
}

#######################################

# kill show2.elf if running
killall -9 show2.elf > /dev/null 2>&1 || true

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH" && sync
while [ -f "$EXEC_PATH" ]; do
	echo "launch: starting nextui.elf $(date)" >> "$LAUNCH_LOG"
	nextui.elf > "$LOGS_PATH/nextui.txt" 2>&1
	EXIT_CODE=$?
	echo "launch: nextui.elf exited $EXIT_CODE $(date)" >> "$LAUNCH_LOG"
	# default launched paks to performance, they can change it themselves after launch if they want
	sh "$SYSTEM_PATH/bin/governor.sh" "performance"

	if [ -f "$NEXT_PATH" ]; then
		CMD="$(cat "$NEXT_PATH")"
		parse_hook_cmd "$CMD"
		"$SYSTEM_PATH/bin/run_hooks.sh" pre-launch.d
		eval "$CMD"
		"$SYSTEM_PATH/bin/run_hooks.sh" post-launch.d
		rm -f "$NEXT_PATH"
		# reset to performance when exiting, UI will reset to auto if needed
		sh "$SYSTEM_PATH/bin/governor.sh" "performance"
	fi

	if [ -f "/tmp/poweroff" ]; then
		poweroff_next || poweroff
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		reboot_next || reboot
		exit 0
	fi
done

poweroff_next || poweroff

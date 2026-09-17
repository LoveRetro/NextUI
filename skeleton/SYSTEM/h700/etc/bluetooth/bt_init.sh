#!/bin/sh

# H700 stock firmware attaches the RTL8821CS Bluetooth UART from the stock
# frontend via setBluetooth.sh. NextUI replaces that frontend, so it must run
# the vendor attach path itself before starting BlueZ.

if [ -z "$RGXX_MODEL" ]; then
	RGXX_MODEL="$(strings /mnt/vendor/bin/dmenu.bin 2>/dev/null | grep -m1 '^RG')"
fi
DEVICE_NAME="Anbernic ${RGXX_MODEL:-RG XX} (NextUI)"
VENDOR_BT_SCRIPT="/mnt/vendor/ctrl/setBluetooth.sh"
BTCTL="/usr/bin/bluetoothctl"
BTCTL_TIMEOUT=5
BLUEALSA="/usr/bin/bluealsa"
HCI_PATH="/sys/class/bluetooth/hci0"
VENDOR_LOCK="/tmp/.init_bt"
NEXTUI_LOCK="/tmp/nextui-bt-init.lock"
HCI_LOG="/tmp/nextui-rtk_hciattach.log"
BLUEALSA_LOG="/tmp/nextui-bluealsa.log"
LOG_DIR="${LOGS_PATH:-/mnt/SDCARD/.userdata/h700/logs}"
LOG_FILE="$LOG_DIR/bluetooth.txt"

mkdir -p "$LOG_DIR" 2>/dev/null || true

log() {
	bt_message="$(date '+%Y-%m-%d %H:%M:%S') bt_init: $*"
	printf '%s\n' "$bt_message" >> "$LOG_FILE" 2>/dev/null || true
	printf '%s\n' "$bt_message" >&2
}

release_lock() {
	rm -rf "$NEXTUI_LOCK" 2>/dev/null || true
}

acquire_lock() {
	bt_lock_tries=0
	while ! mkdir "$NEXTUI_LOCK" 2>/dev/null; do
		if [ -r "$NEXTUI_LOCK/pid" ]; then
			bt_lock_pid="$(cat "$NEXTUI_LOCK/pid" 2>/dev/null)"
			if [ -n "$bt_lock_pid" ] && ! kill -0 "$bt_lock_pid" 2>/dev/null; then
				log "Removing stale operation lock from pid $bt_lock_pid"
				release_lock
				continue
			fi
		fi

		bt_lock_tries=$((bt_lock_tries + 1))
		if [ "$bt_lock_tries" -ge 10 ]; then
			log "Timed out waiting for another Bluetooth operation"
			return 1
		fi
		sleep 1
	done

	printf '%s\n' "$$" > "$NEXTUI_LOCK/pid"
	trap release_lock 0
	trap 'release_lock; exit 1' 1 2 15
	return 0
}

unblock_bt() {
	rfkill unblock bluetooth >> "$LOG_FILE" 2>&1 || true
}

block_bt() {
	rfkill block bluetooth >> "$LOG_FILE" 2>&1 || true
}

run_btctl_bounded() {
	if command -v timeout >/dev/null 2>&1; then
		timeout "$BTCTL_TIMEOUT" "$BTCTL" "$@"
	else
		"$BTCTL" "$@"
	fi
}

wait_for_hci() {
	bt_hci_tries=0
	while [ ! -d "$HCI_PATH" ] && [ "$bt_hci_tries" -lt 7 ]; do
		bt_hci_tries=$((bt_hci_tries + 1))
		sleep 1
	done
	[ -d "$HCI_PATH" ]
}

attach_hci() {
	[ -d "$HCI_PATH" ] && return 0

	if [ ! -x "$VENDOR_BT_SCRIPT" ]; then
		log "Vendor Bluetooth script is missing or not executable: $VENDOR_BT_SCRIPT"
		return 1
	fi

	# Recover from an interrupted init: the vendor script refuses to attach when
	# its tmpfs lock exists, even if rtk_hciattach is no longer running.
	if pidof rtk_hciattach >/dev/null 2>&1; then
		log "rtk_hciattach is running without hci0; restarting it"
		killall rtk_hciattach >> "$LOG_FILE" 2>&1 || true
		sleep 1
	fi
	rm -f "$VENDOR_LOCK"

	log "Attaching stock Bluetooth UART via $VENDOR_BT_SCRIPT"
	: > "$HCI_LOG"
	if ! "$VENDOR_BT_SCRIPT" init >> "$HCI_LOG" 2>&1; then
		log "Vendor Bluetooth init command failed"
		tail -n 80 "$HCI_LOG" >> "$LOG_FILE" 2>&1 || true
		return 1
	fi

	if ! wait_for_hci; then
		log "Timed out waiting for $HCI_PATH"
		tail -n 80 "$HCI_LOG" >> "$LOG_FILE" 2>&1 || true
		ps | grep rtk_hciattach | grep -v grep >> "$LOG_FILE" 2>&1 || true
		return 1
	fi

	log "Bluetooth adapter appeared as hci0"
	return 0
}

start_bluez() {
	if systemctl is-active --quiet bluetooth 2>/dev/null; then
		# BlueZ may have started before hci0 existed. Restart it once after a new
		# attach so its initial adapter state is deterministic.
		if [ "$bt_attached_now" = "1" ]; then
			log "Restarting stock bluetooth.service after HCI attach"
			systemctl restart bluetooth >> "$LOG_FILE" 2>&1 || return 1
		fi
	else
		log "Starting stock bluetooth.service"
		systemctl start bluetooth >> "$LOG_FILE" 2>&1 || return 1
	fi
	return 0
}

wait_for_bluez_adapter() {
	bt_bluez_tries=0
	while [ "$bt_bluez_tries" -lt 5 ]; do
		if "$BTCTL" show >/dev/null 2>&1; then
			return 0
		fi
		bt_bluez_tries=$((bt_bluez_tries + 1))
		sleep 1
	done
	return 1
}

power_on_adapter() {
	bt_power_tries=0
	while [ "$bt_power_tries" -lt 5 ]; do
		if "$BTCTL" show 2>/dev/null | grep -q 'Powered: yes'; then
			return 0
		fi
		if "$BTCTL" power on >> "$LOG_FILE" 2>&1; then
			sleep 1
			if "$BTCTL" show 2>/dev/null | grep -q 'Powered: yes'; then
				return 0
			fi
		fi
		bt_power_tries=$((bt_power_tries + 1))
		sleep 1
	done
	return 1
}

bluealsa_dbus_ready() {
	if command -v busctl >/dev/null 2>&1; then
		busctl --system list 2>/dev/null | grep -q 'org\.bluealsa'
	else
		pidof bluealsa >/dev/null 2>&1
	fi
}

start_bluealsa() {
	if [ ! -x "$BLUEALSA" ]; then
		log "Stock bluealsa is missing or not executable; Bluetooth audio will be disabled"
		return 1
	fi

	if pidof bluealsa >/dev/null 2>&1; then
		if bluealsa_dbus_ready; then
			return 0
		fi
		log "bluealsa is running without its D-Bus service; restarting it"
		killall bluealsa >> "$LOG_FILE" 2>&1 || true
		sleep 1
	fi

	bt_bluealsa_version="$($BLUEALSA --version 2>&1)"
	if [ $? -ne 0 ]; then
		log "Stock bluealsa cannot run: $bt_bluealsa_version"
		return 1
	fi
	log "Starting bluealsa $bt_bluealsa_version with A2DP source"
	: > "$BLUEALSA_LOG"
	# Keep BlueALSA's default volume mode. BaseOS BlueZ does not expose the
	# transport Volume property early enough for --a2dp-volume, leaving some
	# headsets (including AirPods 4) at absolute volume zero. The default mode
	# publishes the mixer control that libmsettings updates after connection.
	"$BLUEALSA" -p a2dp-source --initial-volume=100 < /dev/null >> "$BLUEALSA_LOG" 2>&1 &
	bt_bluealsa_pid=$!

	bt_bluealsa_tries=0
	while [ "$bt_bluealsa_tries" -lt 5 ]; do
		if ! kill -0 "$bt_bluealsa_pid" 2>/dev/null; then
			log "bluealsa exited during startup"
			tail -n 80 "$BLUEALSA_LOG" >> "$LOG_FILE" 2>&1 || true
			return 1
		fi
		if bluealsa_dbus_ready; then
			log "Bluetooth A2DP source is ready"
			return 0
		fi
		bt_bluealsa_tries=$((bt_bluealsa_tries + 1))
		sleep 1
	done

	log "Timed out waiting for the bluealsa D-Bus service"
	tail -n 80 "$BLUEALSA_LOG" >> "$LOG_FILE" 2>&1 || true
	kill "$bt_bluealsa_pid" 2>/dev/null || true
	return 1
}

start_bt() {
	log "Starting Bluetooth for ${RGXX_MODEL:-unknown H700 model}"

	if [ ! -x "$VENDOR_BT_SCRIPT" ]; then
		log "Vendor Bluetooth script is missing or not executable: $VENDOR_BT_SCRIPT"
		return 1
	fi
	if [ ! -x "$BTCTL" ]; then
		log "Stock bluetoothctl is missing or not executable: $BTCTL"
		return 1
	fi
	btctl_version="$($BTCTL --version 2>&1)"
	if [ $? -ne 0 ]; then
		log "Stock bluetoothctl cannot run: $btctl_version"
		return 1
	fi
	log "Using stock $btctl_version"

	unblock_bt

	bt_attached_now=0
	if [ ! -d "$HCI_PATH" ]; then
		attach_hci || return 1
		bt_attached_now=1
	fi

	if ! "$VENDOR_BT_SCRIPT" enable >> "$LOG_FILE" 2>&1; then
		log "Vendor Bluetooth enable command failed"
		return 1
	fi

	start_bluez || {
		log "Failed to start stock bluetooth.service"
		return 1
	}

	if ! wait_for_bluez_adapter; then
		log "BlueZ did not expose hci0 through bluetoothctl"
		return 1
	fi

	if ! power_on_adapter; then
		log "Failed to power on the BlueZ adapter"
		return 1
	fi
	"$BTCTL" discoverable on >> "$LOG_FILE" 2>&1 || log "Failed to make the adapter discoverable"
	"$BTCTL" pairable on >> "$LOG_FILE" 2>&1 || log "Failed to make the adapter pairable"
	"$BTCTL" system-alias "$DEVICE_NAME" >> "$LOG_FILE" 2>&1 || log "Failed to set the adapter alias"

	if ! "$BTCTL" show 2>/dev/null | grep -q 'Powered: yes'; then
		log "Bluetooth adapter exists but is not powered"
		return 1
	fi
	if ! "$BTCTL" show 2>/dev/null | grep -q 'Pairable: yes'; then
		log "Adapter is powered; Settings will make it pairable while its persistent agent is open"
	fi
	start_bluealsa || log "Controller Bluetooth is ready, but Bluetooth audio is unavailable"

	log "Bluetooth is ready"
	return 0
}

stop_bt() {
	log "Stopping Bluetooth"

	# Settings may still have discovery clients running while the toggle is being
	# changed. Clear them before teardown so they cannot hold open a stale D-Bus
	# request after BlueZ exits.
	killall bluetoothctl 2>/dev/null || true

	# BaseOS starts BlueZ on demand. When Bluetooth is already off, invoking
	# bluetoothctl without bluetoothd can block forever and retain NEXTUI_LOCK,
	# preventing the next start operation. Only ask a live daemon to stop scanning
	# and power down, and bound both best-effort requests in case it is unhealthy.
	if [ -x "$BTCTL" ] && pidof bluetoothd >/dev/null 2>&1; then
		run_btctl_bounded scan off >> "$LOG_FILE" 2>&1 || true
		run_btctl_bounded power off >> "$LOG_FILE" 2>&1 || true
	fi
	killall bluetoothctl 2>/dev/null || true

	# Stop the stock A2DP media endpoint before stopping BlueZ.
	killall bluealsa 2>/dev/null || true

	systemctl stop bluetooth >> "$LOG_FILE" 2>&1 || killall bluetoothd 2>/dev/null || true
	hciconfig hci0 down >> "$LOG_FILE" 2>&1 || true
	if pidof rtk_hciattach >/dev/null 2>&1; then
		killall rtk_hciattach >> "$LOG_FILE" 2>&1 || true
		sleep 1
	fi
	rm -f "$VENDOR_LOCK"
	block_bt

	log "Bluetooth is stopped"
	return 0
}

if ! acquire_lock; then
	exit 1
fi

case "$1" in
	start|"")
		start_bt
		;;
	stop)
		stop_bt
		;;
	restart)
		stop_bt && start_bt
		;;
	*)
		echo "Usage: $0 {start|stop|restart}"
		exit 1
		;;
esac

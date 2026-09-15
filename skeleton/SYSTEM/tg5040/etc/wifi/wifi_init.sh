#!/bin/sh

WIFI_INTERFACE="wlan0"
WPA_SUPPLICANT_CONF="${WPA_SUPPLICANT_CONF:-/etc/wifi/wpa_supplicant.conf}"
WIFI_SOCKET_PATH="${WIFI_SOCKET_PATH:-/etc/wifi/sockets}"
WIFI_LOCK_PATH="${WIFI_LOCK_PATH:-/tmp/nextui-wifi.lock}"
WIFI_NET_PATH="${WIFI_NET_PATH:-/sys/class/net}"
WPA_SUPPLICANT_INIT="${WPA_SUPPLICANT_INIT:-/etc/init.d/wpa_supplicant}"
RFKILL_BIN="${RFKILL_BIN:-rfkill.elf}"
lock_held=

release_lock() {
	if [ -n "$lock_held" ]; then
		rm -f "$WIFI_LOCK_PATH/pid"
		rmdir "$WIFI_LOCK_PATH" 2>/dev/null
		lock_held=
	fi
}

acquire_lock() {
	attempt=0
	while ! mkdir "$WIFI_LOCK_PATH" 2>/dev/null; do
		owner=
		if [ -r "$WIFI_LOCK_PATH/pid" ]; then
			read -r owner < "$WIFI_LOCK_PATH/pid"
		fi

		case "$owner" in
			''|*[!0-9]*)
				;;
			*)
				if ! kill -0 "$owner" 2>/dev/null; then
					rm -f "$WIFI_LOCK_PATH/pid"
					rmdir "$WIFI_LOCK_PATH" 2>/dev/null
					continue
				fi
				;;
		esac

		attempt=$((attempt + 1))
		if [ "$attempt" -ge 50 ]; then
			echo "Wifi operation is already in progress" >&2
			return 1
		fi
		usleep 100000
	done

	echo "$$" > "$WIFI_LOCK_PATH/pid"
	lock_held=1
}

wait_for_interface() {
	attempt=0
	while [ ! -d "$WIFI_NET_PATH/$WIFI_INTERFACE" ]; do
		attempt=$((attempt + 1))
		if [ "$attempt" -ge 50 ]; then
			return 1
		fi
		usleep 100000
	done
}

wait_for_supplicant() {
	attempt=0
	while ! pidof wpa_supplicant > /dev/null 2>&1; do
		attempt=$((attempt + 1))
		if [ "$attempt" -ge 30 ]; then
			return 1
		fi
		usleep 100000
	done
}

shutdown_requested() {
	[ -e /tmp/poweroff ] || [ -e /tmp/reboot ]
}

start() {
	if shutdown_requested; then
		echo "Refusing to start Wifi during shutdown" >&2
		return 1
	fi

	acquire_lock || return 1

	# Unblock wifi via rfkill
	if ! "$RFKILL_BIN" unblock wifi 2>/dev/null; then
		echo "Failed to unblock Wifi" >&2
		return 1
	fi
	
	# Create default wpa_supplicant.conf if it doesn't exist
	mkdir -p "$WIFI_SOCKET_PATH"
	if [ ! -f "$WPA_SUPPLICANT_CONF" ]; then
		mkdir -p "$(dirname "$WPA_SUPPLICANT_CONF")"
		cat > "$WPA_SUPPLICANT_CONF" << 'EOF'
# cat /etc/wifi/wpa_supplicant.conf
ctrl_interface=/etc/wifi/sockets
disable_scan_offload=1
update_config=1
wowlan_triggers=any

EOF
	fi

	if ! wait_for_interface; then
		echo "Wifi interface did not appear" >&2
		"$RFKILL_BIN" block wifi 2>/dev/null
		return 1
	fi

	if ! pidof wpa_supplicant > /dev/null 2>&1; then
		"$WPA_SUPPLICANT_INIT" start
	fi

	if ! wait_for_supplicant; then
		echo "wpa_supplicant did not start" >&2
		"$RFKILL_BIN" block wifi 2>/dev/null
		return 1
	fi

	# Start DHCP client to obtain IP address
	if ! pidof udhcpc > /dev/null 2>&1; then	
		udhcpc -i "$WIFI_INTERFACE" -b > /dev/null 2>&1 &
	fi
}

stop() {
	acquire_lock || return 1
	stop_failed=

	"$WPA_SUPPLICANT_INIT" stop
	attempt=0
	while pidof wpa_supplicant > /dev/null 2>&1; do
		attempt=$((attempt + 1))
		if [ "$attempt" -ge 20 ]; then
			killall wpa_supplicant 2>/dev/null
			usleep 200000
			if pidof wpa_supplicant > /dev/null 2>&1; then
				stop_failed=1
			fi
			break
		fi
		usleep 100000
	done

	if ! "$RFKILL_BIN" block wifi; then
		stop_failed=1
	fi

	# Kill DHCP client
	killall udhcpc 2>/dev/null

	[ -z "$stop_failed" ]
}

trap 'release_lock' EXIT
trap 'exit 1' HUP INT TERM

case "$1" in
  start|"")
        start
        ;;
  stop)
        stop
        ;;
  *)
        echo "Usage: $0 {start|stop}"
        exit 1
esac

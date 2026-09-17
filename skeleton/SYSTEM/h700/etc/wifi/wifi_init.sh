#!/bin/sh

WIFI_INTERFACE="${WIFI_INTERFACE:-wlan0}"
SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"
USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/h700}"
WIFI_STATE_DIR="$USERDATA_PATH/wifi"
WIFI_RUNTIME_DIR="/tmp/wifi"
WIFI_SOCK_DIR="$WIFI_RUNTIME_DIR/sockets"
WPA_SUPPLICANT_CONF="$WIFI_STATE_DIR/wpa_supplicant.conf"
WPA_ACTION_SCRIPT="$WIFI_RUNTIME_DIR/wpa_action.sh"

write_default_config() {
	mkdir -p "$WIFI_STATE_DIR" "$WIFI_SOCK_DIR"
	if [ ! -f "$WPA_SUPPLICANT_CONF" ]; then
		cat > "$WPA_SUPPLICANT_CONF" << EOF
ctrl_interface=$WIFI_SOCK_DIR
disable_scan_offload=1
update_config=1
wowlan_triggers=any

EOF
	fi
}

write_action_script() {
	mkdir -p "$WIFI_RUNTIME_DIR"
	cat > "$WPA_ACTION_SCRIPT" << 'EOF'
#!/bin/sh

IFACE="$1"
EVENT="$2"

case "$EVENT" in
	CONNECTED)
		if command -v dhclient >/dev/null 2>&1; then
			dhclient -r "$IFACE" >/dev/null 2>&1 || true
			dhclient -nw "$IFACE" >/dev/null 2>&1 || true
		elif command -v udhcpc >/dev/null 2>&1; then
			killall udhcpc 2>/dev/null || true
			udhcpc -i "$IFACE" -b >/dev/null 2>&1 || true
		fi
		;;
	DISCONNECTED)
		if command -v dhclient >/dev/null 2>&1; then
			dhclient -r "$IFACE" >/dev/null 2>&1 || true
		fi
		killall udhcpc 2>/dev/null || true
		;;
esac
EOF
	chmod +x "$WPA_ACTION_SCRIPT"
}

start_dhcp() {
	if command -v dhclient >/dev/null 2>&1; then
		dhclient -nw "$WIFI_INTERFACE" >/dev/null 2>&1 || true
	elif command -v udhcpc >/dev/null 2>&1; then
		udhcpc -i "$WIFI_INTERFACE" -b >/dev/null 2>&1 || true
	fi
}

stop_dhcp() {
	if command -v dhclient >/dev/null 2>&1; then
		dhclient -r "$WIFI_INTERFACE" >/dev/null 2>&1 || true
	fi
	killall udhcpc 2>/dev/null || true
}

wait_for_interface() {
	# The driver may still be initializing the radio when the frontend starts.
	# This runs in the background from launch.sh, so waiting costs the UI nothing.
	n=0
	while [ ! -d "/sys/class/net/$WIFI_INTERFACE" ] && [ "$n" -lt 100 ]; do
		sleep 0.25
		n=$((n + 1))
	done
}

start() {
	systemctl stop NetworkManager 2>/dev/null || true
	systemctl stop wpa_supplicant "wpa_supplicant@$WIFI_INTERFACE.service" 2>/dev/null || true
	wait_for_interface
	rfkill unblock wifi 2>/dev/null || true
	ip link set "$WIFI_INTERFACE" up 2>/dev/null || true
	write_default_config
	write_action_script

	killall wpa_cli 2>/dev/null || true
	killall wpa_supplicant 2>/dev/null || true
	wpa_supplicant -B -i "$WIFI_INTERFACE" -c "$WPA_SUPPLICANT_CONF" -C "$WIFI_SOCK_DIR" >/dev/null 2>&1
	wpa_cli -B -p "$WIFI_SOCK_DIR" -i "$WIFI_INTERFACE" -a "$WPA_ACTION_SCRIPT" >/dev/null 2>&1 || true
	start_dhcp
}

stop() {
	wpa_cli -p "$WIFI_SOCK_DIR" -i "$WIFI_INTERFACE" terminate >/dev/null 2>&1 || true
	killall wpa_cli 2>/dev/null || true
	systemctl stop wpa_supplicant "wpa_supplicant@$WIFI_INTERFACE.service" 2>/dev/null || true
	killall wpa_supplicant 2>/dev/null || true
	stop_dhcp
	ip link set "$WIFI_INTERFACE" down 2>/dev/null || true
	rfkill block wifi 2>/dev/null || true
}

case "$1" in
	start|"")
		start
		;;
	stop)
		stop
		;;
	restart)
		stop
		sleep 1
		start
		;;
	*)
		echo "Usage: $0 {start|stop|restart}"
		exit 1
		;;
esac

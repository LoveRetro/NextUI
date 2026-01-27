#!/bin/sh

WIFI_INTERFACE="wlan0"
WPA_SUPPLICANT_CONF="/etc/wifi/wpa_supplicant/wpa_supplicant.conf"

start() {
	# Load WiFi driver module if not loaded
	if ! lsmod | grep -q aic8800_fdrv; then
		modprobe aic8800_fdrv.ko 2>/dev/null
		sleep 0.5
	fi
	
	# Unblock wifi via rfkill
	rfkill unblock wifi 2>/dev/null
	
	# Bring up the interface
	ip link set $WIFI_INTERFACE up 2>/dev/null
	
	# Create default wpa_supplicant.conf if it doesn't exist
	if [ ! -f "$WPA_SUPPLICANT_CONF" ]; then
		mkdir -p "$(dirname "$WPA_SUPPLICANT_CONF")"
		cat > "$WPA_SUPPLICANT_CONF" << 'EOF'
# cat /etc/wifi/wpa_supplicant/wpa_supplicant.conf
ctrl_interface=/etc/wifi/sockets
disable_scan_offload=1
update_config=1
wowlan_triggers=any

EOF
	fi
	
	# Start wpa_supplicant if not running
	if ! pidof wpa_supplicant > /dev/null 2>&1; then
		wpa_supplicant -B -i $WIFI_INTERFACE -c $WPA_SUPPLICANT_CONF -D nl80211 2>/dev/null
		sleep 0.5
	fi

	# Start DHCP client to obtain IP address
	if ! pidof udhcpc > /dev/null 2>&1; then	
		udhcpc -i $WIFI_INTERFACE -b 2>/dev/null
	fi
}

stop() {
	# Disconnect and disable
	wpa_cli -i $WIFI_INTERFACE disconnect 2>/dev/null
	
	# Bring down interface
	ip link set $WIFI_INTERFACE down 2>/dev/null
	
	# Block wifi to save power
	rfkill block wifi 2>/dev/null

	# Kill wpa_supplicant
	killall wpa_supplicant 2>/dev/null

	# Kill DHCP client
	killall udhcpc 2>/dev/null
}

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
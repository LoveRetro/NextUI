#!/bin/sh

WIFI_INTERFACE="wlan0"
WPA_SUPPLICANT_CONF="/etc/wifi/wpa_supplicant.conf"

start() {
	# Unblock wifi via rfkill
	rfkill.elf unblock wifi 2>/dev/null
	
	# Create default wpa_supplicant.conf if it doesn't exist
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

	/etc/init.d/wpa_supplicant start

	# Start DHCP client to obtain IP address
	if ! pidof udhcpc > /dev/null 2>&1; then	
		udhcpc -i $WIFI_INTERFACE -b 2>/dev/null
	fi
}

stop() {
	/etc/init.d/wpa_supplicant stop

	rfkill.elf block wifi

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
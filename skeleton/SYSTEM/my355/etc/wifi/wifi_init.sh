#!/bin/sh

WIFI_INTERFACE="wlan0"
WPA_SUPPLICANT_CONF="/userdata/cfg/wpa_supplicant.conf"

start() {
	insmod /system/lib/modules/RTL8189FU.ko 2>/dev/null
	rfkill.elf unblock wifi
	/etc/init.d/S36load_wifi_modules start
	/etc/init.d/S40network start
	/etc/init.d/S41dhcpcd start

	# Start wpa_supplicant if not running
	if ! pidof wpa_supplicant > /dev/null 2>&1; then
		wpa_supplicant -B -i $WIFI_INTERFACE -c $WPA_SUPPLICANT_CONF -O /var/run/wpa_supplicant -D nl80211 2>/dev/null
		sleep 0.5
	fi

	# Start DHCP client to obtain IP address
	if ! pidof udhcpc > /dev/null 2>&1; then	
		udhcpc -i $WIFI_INTERFACE -b 2>/dev/null
	fi
}

stop() {
	/etc/init.d/S41dhcpcd stop
	/etc/init.d/S40network stop
	/etc/init.d/S36load_wifi_modules stop

	rfkill.elf block wifi

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
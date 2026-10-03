#!/bin/sh

WIFI_INTERFACE="wlan0"
WPA_SUPPLICANT_CONF="/userdata/cfg/wpa_supplicant.conf"

# The USB wifi adapter is re-enumerated on resume, so wlan0 is gone for a couple
# of seconds. Without waiting, wpa_supplicant binds to a missing interface.
wait_for_interface() {
	i=0
	while [ ! -e "/sys/class/net/$WIFI_INTERFACE" ]; do
		i=$((i + 1))
		if [ $i -gt 50 ]; then # ~10s
			echo "wifi_init: $WIFI_INTERFACE did not appear, continuing anyway" >&2
			return 1
		fi
		sleep 0.2
	done
	return 0
}

start() {
	insmod /system/lib/modules/RTL8189FU.ko 2>/dev/null
	rfkill.elf unblock wifi
	/etc/init.d/S36load_wifi_modules start
	wait_for_interface

	# udhcpc used to run alongside dhcpcd and they fought over wlan0's address;
	# S40network only bounced loopback, /etc/network/interfaces lists only lo.
	/etc/init.d/S41dhcpcd start

	# Start wpa_supplicant if not running
	if ! pidof wpa_supplicant > /dev/null 2>&1; then
		wpa_supplicant -B -i $WIFI_INTERFACE -c $WPA_SUPPLICANT_CONF -O /var/run/wpa_supplicant -D nl80211 2>/dev/null
		sleep 0.5
	fi
}

stop() {
	/etc/init.d/S41dhcpcd stop
	/etc/init.d/S36load_wifi_modules stop

	rfkill.elf block wifi

	# Kill wpa_supplicant
	killall wpa_supplicant 2>/dev/null

	# leftover from older installs
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
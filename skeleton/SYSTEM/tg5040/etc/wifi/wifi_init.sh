#!/bin/sh

start() {
	rfkill.elf unblock wifi

	/etc/init.d/wpa_supplicant start
	
	#wifi_daemon -s &
}

stop() {
	d=`ps | grep wifi_daemon | grep -v grep`
	[ -n "$d" ] && {
		killall wifi_daemon
		sleep 1
	}

	/etc/init.d/wpa_supplicant stop

	rfkill.elf block wifi
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
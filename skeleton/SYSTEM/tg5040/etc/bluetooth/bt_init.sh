#!/bin/sh
# Taken from allwinner/btmanager/config/xradio_bt_init.sh for NextUI
bt_hciattach="hciattach"

start_hci_attach()
{
	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && {
		killall "$bt_hciattach"
	}

	#xradio init
	"$bt_hciattach" -n ttyS1 xradio >/dev/null 2>&1 &

	wait_hci0_count=0
	while true
	do
		[ -d /sys/class/bluetooth/hci0 ] && break
		usleep 100000
		let wait_hci0_count++
		[ $wait_hci0_count -eq 70 ] && {
			echo "bring up hci0 failed"
			exit 1
		}
	done
}

start() {
	rfkill.elf unblock bluetooth

	if [ -d "/sys/class/bluetooth/hci0" ];then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi

    d=`ps | grep bluetoothd | grep -v grep`
	[ -z "$d" ] && {
		/etc/bluetooth/bluetoothd start
		sleep 1
    }

	a=`ps | grep bluealsa | grep -v grep`
	[ -z "$a" ] && {
		bluealsa -p a2dp-source &
		sleep 1
    }

	b=`ps | grep bt_daemon | grep -v grep`
	[ -z "$b" ] && {
		bt_daemon -s &
		# sleep 1
    }

}

ble_start() {
	rfkill.elf unblock bluetooth

	if [ -d "/sys/class/bluetooth/hci0" ];then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi

	hci_is_up=`hciconfig hci0 | grep RUNNING`
	[ -z "$hci_is_up" ] && {
		hciconfig hci0 up
	}

	MAC_STR=`hciconfig | grep "BD Address" | awk '{print $3}'`
	LE_MAC=${MAC_STR/2/C}
	OLD_LE_MAC_T=`cat /sys/kernel/debug/bluetooth/hci0/random_address`
	OLD_LE_MAC=$(echo $OLD_LE_MAC_T | tr [a-z] [A-Z])
	if [ -n "$LE_MAC" ];then
		if [ "$LE_MAC" != "$OLD_LE_MAC" ];then
			hciconfig hci0 lerandaddr $LE_MAC
		else
			echo "the ble random_address has been set."
		fi
	fi
}

stop() {

	b=`ps | grep bt_daemon | grep -v grep`
	[ -n "$b" ] && {
		killall bt_daemon
		#sleep 1
	}

	a=`ps | grep bluealsa | grep -v grep`
	[ -n "$a" ] && {
		killall bluealsa
		sleep 1
	}

	d=`ps | grep bluetoothd | grep -v grep`
	[ -n "$d" ] && {
		killall bluetoothd
		sleep 1
	}

	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && {
		killall "$bt_hciattach"
		sleep 1
	}

	rfkill.elf block bluetooth
	#echo 0 > /sys/class/rfkill/rfkill0/state;
	sleep 1
	echo "stop bluetoothd and hciattach"
}

case "$1" in
  start|"")
        start
        ;;
  stop)
        stop
        ;;
  ble_start)
	    ble_start
		;;
  *)
        echo "Usage: $0 {start|stop}"
        exit 1
esac
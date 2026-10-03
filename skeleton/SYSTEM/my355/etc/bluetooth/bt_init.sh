#!/bin/sh
# Bluetooth initialization script for NextUI
DEVICE_NAME="Miyoo Flip (NextUI)"

# hci0 shows up on its own once rtk_btusb binds the USB BT interface
wait_hci0() {
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

start_bt() {
	# Load BT driver module if not loaded
	if ! lsmod | grep -q rtk_btusb; then
		insmod /lib/modules/rtk_btusb.ko 2>/dev/null
		sleep 0.5
	fi
	
	rfkill.elf unblock bluetooth

	wait_hci0

	# Start bluetooth daemon if not running
    d=`ps | grep bluetoothd | grep -v grep`
	[ -z "$d" ] && {
		/etc/init.d/S40bluetooth start
		sleep 1
    }

	a=`ps | grep bluealsa | grep -v grep`
	[ -z "$a" ] && {
		# bluealsa -p a2dp-source --keep-alive=-1 &
		bluealsa -p a2dp-source &
		sleep 1
		# Power on adapter
		bluetoothctl power on 2>/dev/null
		
		# Set discoverable and pairable
		bluetoothctl discoverable on 2>/dev/null
		bluetoothctl pairable on 2>/dev/null
		
		# Set default agent for automatic pairing (no input/output)
		bluetoothctl agent NoInputNoOutput 2>/dev/null
		bluetoothctl default-agent 2>/dev/null
		
		# Set adapter name
		bluetoothctl system-alias "$DEVICE_NAME" 2>/dev/null
    }
	
}

stop_bt() {
	# stop bluealsa
	killall bluealsa 2>/dev/null

	# Stop bluetooth service
	d=`ps | grep bluetoothd | grep -v grep`
	[ -n "$d" ] && {
		# stop bluetoothctl
		bluetoothctl power off 2>/dev/null
		#bluetoothctl discoverable off 2>/dev/null
		bluetoothctl pairable off 2>/dev/null
		#bluetoothctl remove $(bluetoothctl devices | awk '{print $2}') 2>/dev/null
		killall bluetoothctl 2>/dev/null
		killall bluetoothd
		sleep 1
	}

	hciconfig hci0 down
	t=`ps | grep hcidump | grep -v grep`
	[ -n "$t" ] && {
		killall hcidump
	}

	rfkill.elf block bluetooth
	echo "stop bluetoothd"
}

case "$1" in
	start)
		start_bt
		;;
	stop)
		stop_bt
		;;
	restart)
		stop_bt
		sleep 0.5
		start_bt
		;;
  *)
		echo "Usage: $0 {start|stop|restart}"
		exit 1
		;;
esac

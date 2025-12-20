#!/bin/sh
# Bluetooth initialization script for NextUI

DEVICE_NAME="Trimui Smart Pro S (NextUI)"

start_bt() {
	# Load BT driver module if not loaded
	if ! lsmod | grep -q aic8800_btlpm; then
		modprobe aic8800_btlpm.ko 2>/dev/null
		sleep 0.5
	fi

	# Start bluetooth service if not running
	/etc/bluetooth/bt_init.sh start 2>/dev/null
	#sleep 0.5

	# Start hciattach if not running
    hpid=`pgrep hciattach`
    if [ "$hpid" == "" ] ; then
        hciattach -n ttyAS1 aic &
    fi        

	# Start bluetooth daemon if not running
    d=`ps | grep bluetoothd | grep -v grep`
	[ -z "$d" ] && {
		/etc/bluetooth/bluetoothd start
		sleep 1
    }

	a=`ps | grep bluealsa | grep -v grep`
	[ -z "$a" ] && {
		# bluealsa -p a2dp-source --keep-alive=-1 &
		bluealsa -p a2dp-source &
		sleep 1
    }
	
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

stop_bt() {
	# Stop bluetooth service
	/etc/bluetooth/bt_init.sh stop 2>/dev/null
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

exit 0
#!/bin/sh
# Bluetooth initialization script for NextUI
bt_hciattach="hciattach"
DEVICE_NAME="Trimui Smart Pro S (NextUI)"

reset_bluetooth_power() {
	echo 0 > /sys/class/rfkill/rfkill0/state;
	sleep 1
	echo 1 > /sys/class/rfkill/rfkill0/state;
	sleep 1
}

start_hci_attach() {
	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && {
		killall "$bt_hciattach"
	}

	echo 1 > /proc/bluetooth/sleep/btwrite
	reset_bluetooth_power

	"$bt_hciattach" -n ttyAS1 aic >/dev/null 2>&1 &

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
	# Looks like this also needs the wifi driver module loaded for proper operation
	if ! lsmod | grep -q aic8800_fdrv; then
		modprobe aic8800_fdrv.ko 2>/dev/null
		sleep 0.5
	fi
	if ! lsmod | grep -q aic8800_btlpm; then
		modprobe aic8800_btlpm.ko 2>/dev/null
		sleep 0.5
	fi

	if [ -d "/sys/class/bluetooth/hci0" ];then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi      

	# Allow headsets to auto-reconnect without user re-pairing.
	# Some BT controller firmware never persists link keys to disk;
	# JustWorksRepairing=always lets earbuds re-initiate the bond
	# from their side after a reboot without user interaction.
	if ! grep -q 'JustWorksRepairing = always' /etc/bluetooth/main.conf 2>/dev/null; then
		sed -i 's/#JustWorksRepairing = never/JustWorksRepairing = always/' /etc/bluetooth/main.conf 2>/dev/null
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

		# Proactively reconnect trusted A2DP audio devices after BT restart.
		# Some headphones (e.g. Sony) won't fall back to JustWorks re-pairing
		# when authentication fails; they expect the host to initiate using the
		# stored link key. Runs in background to avoid blocking startup.
		{
			sleep 5
			for dev_dir in /var/lib/bluetooth/*/; do
				for paired_dir in "${dev_dir}"*/; do
					[ -f "${paired_dir}info" ] || continue
					grep -q "^Trusted=true" "${paired_dir}info" || continue
					grep -q "0000110b" "${paired_dir}info" || continue
					mac=$(basename "${paired_dir%/}")
					bluetoothctl connect "$mac" >/dev/null 2>&1
				done
			done
		} &
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

	t=`ps | grep hcidump | grep -v grep`
	[ -n "$t" ] && {
		killall hcidump
	}
	# xr819s_stop
	hciconfig hci0 down
	h=`ps | grep "$bt_hciattach" | grep -v grep`
	[ -n "$h" ] && {
		killall "$bt_hciattach"
		usleep 500000
	}
	echo 0 > /proc/bluetooth/sleep/btwrite
	echo 0 > /sys/class/rfkill/rfkill0/state;
	echo "stop bluetoothd and hciattach"
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
#!/bin/sh
# Taken from allwinner/btmanager/config/xradio_bt_init.sh for NextUI
bt_hciattach="hciattach"
TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
DEVICE_NAME="$TRIMUI_MODEL (NextUI)"

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

start_bt() {
	rfkill.elf unblock bluetooth

	if [ -d "/sys/class/bluetooth/hci0" ];then
		echo "Bluetooth init has been completed!!"
	else
		start_hci_attach
	fi
	
	# Allow headsets to auto-reconnect without user re-pairing.
	# XRadio BT firmware sets store_hint=0, so link keys are never
	# persisted; JustWorksRepairing=always lets earbuds re-initiate
	# the bond from their side after a reboot.
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
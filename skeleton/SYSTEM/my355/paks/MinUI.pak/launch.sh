#!/bin/sh

export PLATFORM="my355"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export HOME="$USERDATA_PATH"

#######################################

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export DEVICE="my355"

export IS_NEXT="yes"

#######################################

#headphone jack
echo 150 > /sys/class/gpio/export
echo -n in > /sys/class/gpio/gpio150/direction

#motor
echo 20 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio20/direction
echo -n 0 > /sys/class/gpio/gpio20/value

#keyboard
echo 0 > /sys/class/miyooio_chr_dev/joy_type

#screen adjustment for now here
modetest -M rockchip -w 179:hue:60
modetest -M rockchip -w 179:saturation:60

# disable system-level lid handling
mv /dev/input/event1 /dev/input/event1.disabled

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/miyoo/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/miyoo/bin:/usr/miyoo/sbin:$PATH

#led
echo 100 > /sys/class/leds/work/brightness

# start stock gpio input daemon
mkdir -p /tmp/miyoo_inputd
miyoo_inputd &

echo userspace > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
CPU_PATH=/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed
CPU_SPEED_PERF=1992000
echo $CPU_SPEED_PERF > $CPU_PATH

keymon.elf & # &> $SDCARD_PATH/keymon.txt &
batmon.elf & # &> $SDCARD_PATH/batmon.txt &

# start fresh, will be populated on the next connect
rm -f $USERDATA_PATH/.asoundrc
audiomon.elf & # &> $SDCARD_PATH/audiomon.txt &

# wifi handling
wifion=$(nextval.elf wifi | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
if [ "$wifion" -eq 0 ]; then
	$SYSTEM_PATH/etc/wifi/wifi_init.sh stop > /dev/null 2>&1 &
else
	$SYSTEM_PATH/etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
fi

# BT handling
bluetoothon=$(nextval.elf bluetooth | sed -n 's/.*"bluetooth": \([0-9]*\).*/\1/p')
if [ "$bluetoothon" -eq 0 ]; then
	$SYSTEM_PATH/etc/bluetooth/bt_init.sh stop > /dev/null 2>&1 &
else
	$SYSTEM_PATH/etc/bluetooth/bt_init.sh start > /dev/null 2>&1 &
fi

#######################################

AUTO_PATH="$USERDATA_PATH/auto.sh"
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH" # > $LOGS_PATH/auto.txt 2>&1
fi

cd $(dirname "$0")

#######################################

# kill show2.elf if running
killall -9 show2.elf > /dev/null 2>&1

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH" && sync

# Enable audio idk why have to do this now
tinymix set 1 SPK

while [ -f "$EXEC_PATH" ]; do
	nextui.elf &> $LOGS_PATH/nextui.txt
	echo $CPU_SPEED_PERF > $CPU_PATH
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		echo $CPU_SPEED_PERF > $CPU_PATH
	fi

	if [ -f "/tmp/poweroff" ]; then
		shutdown
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		reboot
		exit 0
	fi
done

shutdown

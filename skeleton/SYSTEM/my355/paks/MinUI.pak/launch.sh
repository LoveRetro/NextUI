#!/bin/sh

export PLATFORM="my355"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export SAVES_PATH="$SDCARD_PATH/Saves"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

#######################################

export PATH=$SYSTEM_PATH/bin:/usr/miyoo/bin:/usr/miyoo/sbin:$PATH
export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/miyoo/lib:$LD_LIBRARY_PATH

echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" > /mnt/SDCARD/librarypath.txt
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

#led
echo 100 > /sys/class/leds/work/brightness

mkdir -p /tmp/miyoo_inputd
miyoo_inputd &

# disable system-level lid handling
mv /dev/input/event1 /dev/input/event1.disabled

# TODO: disable network stuff

#######################################

echo userspace > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
CPU_PATH=/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed
CPU_SPEED_PERF=1992000
echo $CPU_SPEED_PERF > $CPU_PATH

#######################################

# wifi handling
/etc/init.d/S36load_wifi_modules start
/etc/init.d/S40network start
/etc/init.d/S41dhcpcd start

wpa_supplicant -B -i wlan0 -c /userdata/cfg/wpa_supplicant.conf
dhcpcd wlan0

keymon.elf &> $SDCARD_PATH/keymon.txt &
batmon.elf & # &> $SDCARD_PATH/batmon.txt &

#######################################

mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"
AUTO_PATH="$USERDATA_PATH/auto.sh"
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH" # > $LOGS_PATH/auto.txt 2>&1
fi

cd $(dirname "$0")

#######################################

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH" && sync

# Enable audio idk why have to do this now
tinymix set 1 SPK

while [ -f "$EXEC_PATH" ]; do
	nextui.elf &> $LOGS_PATH/nextui.txt
	echo $CPU_SPEED_PERF > $CPU_PATH
	echo `date +'%F %T'` > "$DATETIME_PATH"
	sync
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		echo $CPU_SPEED_PERF > $CPU_PATH
		echo `date +'%F %T'` > "$DATETIME_PATH"
		sync
	fi
done

shutdown

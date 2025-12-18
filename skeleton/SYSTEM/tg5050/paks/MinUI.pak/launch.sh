#!/bin/sh
# MinUI.pak

# recover from readonly SD card -------------------------------
# touch /mnt/writetest
# sync
# if [ -f /mnt/writetest ] ; then
# 	rm -f /mnt/writetest
# else
# 	e2fsck -p /dev/root > /mnt/SDCARD/RootRecovery.txt
# 	reboot
# fi

export PLATFORM="tg5050"
export SDCARD_PATH="/mnt/SDCARD"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

#######################################

if [ -f "/tmp/poweroff" ]; then
	poweroff
	exit 0
fi
if [ -f "/tmp/reboot" ]; then
	reboot
	exit 0
fi

#######################################

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Smart Pro S" ]; then
	export DEVICE="smartpros"
fi

export IS_NEXT="yes"

#######################################

echo 1 > /sys/class/drm/card0-DSI-1/rotate
echo 1 > /sys/class/drm/card0-DSI-1/force_rotate

# taken from stock launch sequence
sync
echo 3 > /proc/sys/vm/drop_caches
sync

#5V enable
# echo 335 > /sys/class/gpio/export
# echo -n out > /sys/class/gpio/gpio335/direction
# echo -n 1 > /sys/class/gpio/gpio335/value

#fan off
#echo 0 > /sys/class/thermal/cooling_device0/cur_state 
# pretty much silent to me, and better that heat death
echo 13 > /sys/class/thermal/cooling_device0/cur_state

#rumble motor PH12
echo 236 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio236/direction
echo -n 0 > /sys/class/gpio/gpio236/value

#Left/Right Pad PK12/PK16 , run in trimui_inputd
# echo 332 > /sys/class/gpio/export
# echo -n out > /sys/class/gpio/gpio332/direction
# echo -n 1 > /sys/class/gpio/gpio332/value

# echo 336 > /sys/class/gpio/export
# echo -n out > /sys/class/gpio/gpio336/direction
# echo -n 1 > /sys/class/gpio/gpio336/value

#DIP Switch PL11 , run in trimui_inputd
# echo 363 > /sys/class/gpio/export
# echo -n in > /sys/class/gpio/gpio363/direction

#syslogd -S

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/trimui/bin:$PATH

echo before leds `cat /proc/uptime` >> /tmp/nextui_boottime

# leds_off
echo 0 > /sys/class/led_anim/max_scale

# Disable sleep from inputd until it is patched
#touch /tmp/stay_awake
#touch /tmp/stay_alive

# start gpio input daemon
trimui_inputd &

echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo ondemand > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 1200000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo 408000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
echo 1200000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq

# little Cortex-A55 CPU0 - 408Mhz to 1416Mhz
echo 1 > /sys/devices/system/cpu/cpu0/online
echo 1 > /sys/devices/system/cpu/cpu1/online

echo 0 > /sys/devices/system/cpu/cpu3/online
echo 0 > /sys/devices/system/cpu/cpu2/online

# big Cortex-A55 CPU4 - 408Mhz to 2160Mhz
echo 1 > /sys/devices/system/cpu/cpu4/online

echo 0 > /sys/devices/system/cpu/cpu7/online
echo 0 > /sys/devices/system/cpu/cpu6/online
echo 0 > /sys/devices/system/cpu/cpu5/online

# stuff and strings todo
#/sys/class/backlight/backlight0/brightness
#set cpu fan level %s
#/sys/class/thermal/cooling_device0/cur_state
#/sys/class/power_supply/axp2202-battery/capacity
#/sys/class/thermal/thermal_zone0/temp
#/sys/class/thermal/thermal_zone1/temp
#/sys/class/thermal/thermal_zone2/temp

#echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
#CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
#CPU_SPEED_PERF=2000000
#echo $CPU_SPEED_PERF > $CPU_PATH

keymon.elf & # &> $SDCARD_PATH/keymon.txt &
batmon.elf & # &> $SDCARD_PATH/batmon.txt &

# TODO
# load bt/wifi modules
# thermald for fan control

echo before modprobe `cat /proc/uptime` >> /tmp/nextui_boottime
modprobe aic8800_fdrv.ko
modprobe aic8800_btlpm.ko
echo after modprobe `cat /proc/uptime` >> /tmp/nextui_boottime

# start fresh, will be populated on the next connect
#rm -f $USERDATA_PATH/.asoundrc
#audiomon.elf & # &> $SDCARD_PATH/audiomon.txt &

# BT handling
# on by default, disable based on systemval setting
# TODO: still the case here, do we need to handle the off state explicitly?
bluetoothon=$(nextval.elf bluetooth | sed -n 's/.*"bluetooth": \([0-9]*\).*/\1/p')
if [ "$bluetoothon" -eq 0 ]; then
	/etc/bluetooth/bt_init.sh stop # > /dev/null 2>&1 &
else
	/etc/bluetooth/bt_init.sh start #  > /dev/null 2>&1 &
	hpid=`pgrep hciattach`
    if [ "$hpid" == "" ] ; then
        hciattach -n ttyAS1 aic &
    fi        
    /etc/bluetooth/bluetoothd start
fi
echo after bluetooth `cat /proc/uptime` >> /tmp/nextui_boottime

# wifi handling
# on by default, disable based on systemval setting
# TODO: still the case here, do we need to handle the off state explicitly?
wifion=$(nextval.elf wifi | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
if [ "$wifion" -eq 0 ]; then
	/etc/wifi/wifi_init.sh stop > /dev/null 2>&1 &
else 
	/etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
fi
echo after wifi `cat /proc/uptime` >> /tmp/nextui_boottime

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	echo before auto.sh `cat /proc/uptime` >> /tmp/nextui_boottime
	"$AUTO_PATH"
	echo after auto.sh `cat /proc/uptime` >> /tmp/nextui_boottime
fi

cd $(dirname "$0")

#######################################

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
while [ -f $EXEC_PATH ]; do
	nextui.elf &> $LOGS_PATH/nextui.txt
	#echo $CPU_SPEED_PERF > $CPU_PATH
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		#echo $CPU_SPEED_PERF > $CPU_PATH
	fi

	if [ -f "/tmp/poweroff" ]; then
		poweroff
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		reboot
		exit 0
	fi
done

poweroff # just in case

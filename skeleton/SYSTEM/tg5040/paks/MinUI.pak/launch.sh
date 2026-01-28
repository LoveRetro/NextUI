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

export PLATFORM="tg5040"
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
export HOME="$USERDATA_PATH"

#######################################

if [ -f "/tmp/poweroff" ]; then
	poweroff_next
	exit 0
fi
if [ -f "/tmp/reboot" ]; then
	reboot_next
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
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	export DEVICE="brick"
else
	export DEVICE="smartpro"
fi

export IS_NEXT="yes"

#######################################

##Remove Old Led Daemon
if [ -f "/etc/LedControl" ]; then
	rm -Rf "/etc/LedControl"
fi
if [ -f "/etc/init.d/lcservice" ]; then
	/etc/init.d/lcservice disable
	rm /etc/init.d/lcservice
fi

# clear shadercache unconditionally, until it properly invalidates itself
rm -rf $SDCARD_PATH/.shadercache

#PD11 pull high for VCC-5v
echo 107 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio107/direction
echo -n 1 > /sys/class/gpio/gpio107/value

#rumble motor PH3
echo 227 > /sys/class/gpio/export
echo -n out > /sys/class/gpio/gpio227/direction
echo -n 0 > /sys/class/gpio/gpio227/value

if [ "$TRIMUI_MODEL" = "Trimui Smart Pro" ]; then
	#Left/Right Pad PD14/PD18
	echo 110 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio110/direction
	echo -n 1 > /sys/class/gpio/gpio110/value

	echo 114 > /sys/class/gpio/export
	echo -n out > /sys/class/gpio/gpio114/direction
	echo -n 1 > /sys/class/gpio/gpio114/value
fi

#DIP Switch PH19
echo 243 > /sys/class/gpio/export
echo -n in > /sys/class/gpio/gpio243/direction

syslogd -S

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/trimui/bin:$PATH

# leds_off
echo 0 > /sys/class/led_anim/max_scale
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	echo 0 > /sys/class/led_anim/max_scale_lr
	echo 0 > /sys/class/led_anim/max_scale_f1f2
fi

# start stock gpio input daemon
trimui_inputd &

echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
CPU_SPEED_PERF=2000000
echo $CPU_SPEED_PERF > $CPU_PATH

keymon.elf & # &> $SDCARD_PATH/keymon.txt &
batmon.elf & # &> $SDCARD_PATH/batmon.txt &

# start fresh, will be populated on the next connect
rm -f $USERDATA_PATH/.asoundrc
audiomon.elf & # &> $SDCARD_PATH/audiomon.txt &

# BT handling
# on by default, disable based on systemval setting
bluetoothon=$(nextval.elf bluetooth | sed -n 's/.*"bluetooth": \([0-9]*\).*/\1/p')
# somehow trimui deploys aic?
cp -f $SYSTEM_PATH/etc/bluetooth/bt_init.sh /etc/bluetooth/bt_init.sh
if [ "$bluetoothon" -eq 0 ]; then
	/etc/bluetooth/bt_init.sh stop > /dev/null 2>&1 &
else
	/etc/bluetooth/bt_init.sh start > /dev/null 2>&1 &
fi

# wifi handling
# on by default, disable based on systemval setting
wifion=$(nextval.elf wifi | sed -n 's/.*"wifi": \([0-9]*\).*/\1/p')
cp -f $SYSTEM_PATH/etc/wifi/wifi_init.sh /etc/wifi/wifi_init.sh
if [ "$wifion" -eq 0 ]; then
	/etc/wifi/wifi_init.sh stop > /dev/null 2>&1 &
else 
	/etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
fi

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

cd $(dirname "$0")

#######################################

# kill show2.elf if running
killall -9 show2.elf > /dev/null 2>&1

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
while [ -f $EXEC_PATH ]; do
	nextui.elf &> $LOGS_PATH/nextui.txt
	echo $CPU_SPEED_PERF > $CPU_PATH
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		eval $CMD
		rm -f $NEXT_PATH
		echo $CPU_SPEED_PERF > $CPU_PATH
	fi

	if [ -f "/tmp/poweroff" ]; then
		poweroff_next
		exit 0
	fi
	if [ -f "/tmp/reboot" ]; then
		reboot_next
		exit 0
	fi
done

poweroff_next # just in case

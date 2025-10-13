#!/bin/sh
cd $(dirname "$0")
# chmod a+w /sys/class/led_anim/* >> launch.log

# remove original leddaemon
LCDAEMON_PATH="/etc/LedControl"
rm -R $LCDAEMON_PATH 2> /dev/null
if [ -f /etc/init.d/lcservice ]; then
    /etc/init.d/lcservice disable
    rm /etc/init.d/lcservice 2> /dev/null
fi

cd $(dirname "$0")

TARGET_PATH="/mnt/SDCARD/.userdata/shared/ledsettings.txt"
if [ ! -f "$TARGET_PATH" ]; then
    cp ./ledsettings.txt /mnt/SDCARD/.userdata/shared/ledsettings.txt >> launch.log
    echo "File copied to $TARGET_PATH" >> launch.log
else
    echo "File already exists in TARGET_PATH" >> launch.log
fi

TARGET_PATH="/mnt/SDCARD/.userdata/shared/ledsettings_brick.txt"
if [ ! -f "$TARGET_PATH" ]; then
    cp ./ledsettings_brick.txt /mnt/SDCARD/.userdata/shared/ledsettings_brick.txt >> launch.log
    echo "File copied to $TARGET_PATH" >> launch.log
else
    echo "File already exists in TARGET_PATH" >> launch.log
fi

./ledcontrol.elf > ledcontrol.log 2>&1

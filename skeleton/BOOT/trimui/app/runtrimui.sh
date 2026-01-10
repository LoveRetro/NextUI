#!/bin/sh

# becomes /usr/trimui/bin/runtrimui.sh on tg5040/tg3040/tg5050

#wait for SDCARD mounted
echo before mount `cat /proc/uptime` >> /tmp/nextui_boottime
mounted=`cat /proc/mounts | grep -i SDCARD`
cnt=0
while [ "$mounted" == "" ] && [ $cnt -lt 6 ] ; do
   sleep 0.5
   cnt=`expr $cnt + 1`
   mounted=`cat /proc/mounts | grep -i SDCARD`
done
echo after mount `cat /proc/uptime` >> /tmp/nextui_boottime 

UPDATER_PATH=/mnt/SDCARD/.tmp_update/updater
if [ -f "$UPDATER_PATH" ]; then
	"$UPDATER_PATH"
else
	/usr/trimui/bin/runtrimui-original.sh
fi

#!/bin/sh
# PAYLOAD_VERSION 20260915

# becomes /usr/miyoo/bin/runmiyoo.sh on my355

#wait for sdcard mounted
mounted=`cat /proc/mounts | grep /mnt/sdcard`
cnt=0
while [ "$mounted" == "" ] && [ $cnt -lt 6 ] ; do
   sleep 0.5
   cnt=`expr $cnt + 1`
   mounted=`cat /proc/mounts | grep /mnt/sdcard`
done

if [ -f "/media/sdcard1/.tmp_update/updater" ]; then
    DEV_SD_CARD_A="$(mount | grep /mnt/sdcard | awk '{print $1}')"
    DEV_SD_CARD_B="$(mount | grep /media/sdcard1 | awk '{print $1}')"
    # the right slot may be empty, only swap it back if there is a card
    [ -n "$DEV_SD_CARD_A" ] && umount /mnt/sdcard
    umount /media/sdcard1
    [ -n "$DEV_SD_CARD_A" ] && mount "$DEV_SD_CARD_A" /media/sdcard1
    mount "$DEV_SD_CARD_B" /mnt/sdcard
fi

UPDATER_PATH=/mnt/SDCARD/.tmp_update/updater
if [ -f "$UPDATER_PATH" ]; then
	"$UPDATER_PATH"
else
	/usr/miyoo/bin/runmiyoo-original.sh
fi
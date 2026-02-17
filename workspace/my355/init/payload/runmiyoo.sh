#!/bin/sh

# becomes /usr/miyoo/bin/runmiyoo.sh on my355

#wait for sdcard mounted - we need the primary one at /mnt/sdcard.
#At least on the 2025 firmware, only the right slot will ever be
#bound to /mnt/sdcard, even if you have something in the left slot.
tf1_mounted=`cat /proc/mounts | grep mnt/sdcard`
cnt=0
while [ "$tf1_mounted" == "" ] && [ $cnt -lt 6 ] ; do
   sleep 0.5
   cnt=`expr $cnt + 1`
   tf1_mounted=`cat /proc/mounts | grep mnt/sdcard`
done
tf2_mounted=`cat /proc/mounts | grep media/sdcard1`

if [ "$tf1_mounted" != "" ]; then
   #apparently Miyoo Flip cant kepp its /userdata from corrupting,
   # which breaks all kinds of things (Wifi, BT, NTP) - fun!
   # if we mounted sd correctly, bind mount our own folder over it
   USERDATA_DIR="/mnt/SDCARD/.userdata/my355/userdata"
   if [ ! -d "$USERDATA_DIR" ]; then
      mkdir $USERDATA_DIR
      cp -R /userdata/* $USERDATA_DIR/
      mkdir -p $USERDATA_DIR/bin
      mkdir -p $USERDATA_DIR/bluetooth
      mkdir -p $USERDATA_DIR/cfg
      mkdir -p $USERDATA_DIR/localtime
      mkdir -p $USERDATA_DIR/timezone
      mkdir -p $USERDATA_DIR/lib
      mkdir -p $USERDATA_DIR/lib/bluetooth
      sync
   fi

   if [ ! -f "$USERDATA_DIR/system.json" ]; then
		cat > "$USERDATA_DIR/system.json" << 'EOF'
{
        "vol": 7,
        "keymap": "L2,L,R2,R,X,A,B,Y",
        "mute": 0,
        "bgmvol": 0,
        "brightness": 6,
        "language": "en.lang",
        "hibernate": 0,
        "lumination": 10,
        "hue": 10,
        "saturation": 10,
        "contrast": 10,
        "theme": "",
        "fontsize": 24,
        "audiofix": 1,
        "wifi": 0,
        "runee": 0,
        "turboA": 0,
        "turboB": 0,
        "turboX": 0,
        "turboY": 0,
        "turboL": 0,
        "turboR": 0,
        "turboL2": 0,
        "turboR2": 0,
        "bluetooth": 0
}
EOF
      sync
   fi

   mount --bind $USERDATA_DIR /userdata

   # also fix bluetooth - we cant let it write to sdcard, it creates
   # files by mac address, which arent legal file names on fat32
   mkdir -p /run/bluetooth_fix
   mount --bind /run/bluetooth_fix /userdata/bluetooth
elif [ "$tf2_mounted" != "" ]; then
   # if mounted is empty and wrong_slot is not, the user probably has his
   # nextui card in the left slot.

   # make sure media/sdcard1 is actually a nextui card, we want to use show2.elf
   SYSTEM_PATH="/media/sdcard1/.system/"
   SHOW_PATH="$SYSTEM_PATH/my355/bin/show2.elf"
   if [ -f "$SHOW_PATH" ]; then
      $SHOW_PATH --mode=simple --image="$SYSTEM_PATH/res/logo.png" --text="Please use the right SD slot for NextUI." --logoheight=80 --timeout=60
      poweroff
      while :; do
         sleep 1
      done
   fi
fi

UPDATER_PATH=/mnt/SDCARD/.tmp_update/updater
if [ -f "$UPDATER_PATH" ]; then
	"$UPDATER_PATH"
else
	/usr/miyoo/bin/runmiyoo-original.sh
fi

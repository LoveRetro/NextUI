#!/bin/sh

# becomes /usr/miyoo/bin/runmiyoo.sh on my355

#wait for sdcard mounted
mounted=`cat /proc/mounts | grep sdcard`
cnt=0
while [ "$mounted" == "" ] && [ $cnt -lt 6 ] ; do
   sleep 0.5
   cnt=`expr $cnt + 1`
   mounted=`cat /proc/mounts | grep sdcard`
done

#apparently Miyoo Flip cant kepp its /userdata from corrupting,
# which breaks all kinds of things (Wifi, BT, NTP) - fun!
# if we mounted sd correctly, bind mount our own folder over it
if [ "$mounted" != "" ]; then
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
fi

UPDATER_PATH=/mnt/SDCARD/.tmp_update/updater
if [ -f "$UPDATER_PATH" ]; then
	"$UPDATER_PATH"
else
	/usr/miyoo/bin/runmiyoo-original.sh
fi

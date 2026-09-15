#!/bin/sh
# NOTE: becomes .tmp_update/my355.sh

PLATFORM="my355"
SDCARD_PATH="/mnt/SDCARD"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
PAKZ_PATH="$SDCARD_PATH/*.pakz"
SYSTEM_PATH="$SDCARD_PATH/.system"

export LD_LIBRARY_PATH=/usr/miyoo/lib:$LD_LIBRARY_PATH
export PATH=/usr/miyoo/bin:$PATH

# the hook moves a left slot card (mmcblk2) to /mnt/sdcard, NextUI only supports the right slot
case "$(grep " /mnt/sdcard " /proc/mounts | cut -d' ' -f1)" in
/dev/mmcblk2*)
	cd $(dirname "$0")/$PLATFORM
	./show2.elf --mode=simple --image=logo.png --text="Please use the right SD slot for NextUI." --logoheight=80 --timeout=60
	poweroff
	while :; do
		sleep 1
	done
	;;
esac

# stock /userdata corrupts easily (breaks Wifi, BT, NTP), keep it on the SD card.
# older NextUI hooks already did this from the rootfs.
USERDATA_DIR="$SDCARD_PATH/.userdata/$PLATFORM/userdata"
if ! grep -q " /userdata/bluetooth " /proc/mounts; then
	if [ ! -d "$USERDATA_DIR" ]; then
		mkdir -p "$USERDATA_DIR"
		cp -R /userdata/* "$USERDATA_DIR/"
		mkdir -p "$USERDATA_DIR/bin"
		mkdir -p "$USERDATA_DIR/bluetooth"
		mkdir -p "$USERDATA_DIR/cfg"
		mkdir -p "$USERDATA_DIR/localtime"
		mkdir -p "$USERDATA_DIR/timezone"
		mkdir -p "$USERDATA_DIR/lib/bluetooth"
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

	mount --bind "$USERDATA_DIR" /userdata

	# bluetooth names files by MAC address, which aren't legal on FAT32
	mkdir -p /run/bluetooth_fix
	mount --bind /run/bluetooth_fix /userdata/bluetooth
fi

touch /tmp/fbdisplay_exit
cat /dev/zero > /dev/fb0

# only show splash if either UPDATE_PATH or pakz files exist
SHOW_SPLASH="no"
if [ -f "$UPDATE_PATH" ]; then
	SHOW_SPLASH="yes"
else
	for pakz in $PAKZ_PATH; do
		case "$pakz" in *.tg5040.pakz|*.tg5050.pakz) continue ;; esac
		if [ -e "$pakz" ]; then
			SHOW_SPLASH="yes"
			break
		fi
	done
fi
LOGO_PATH="logo.png"
# If the user put a custom logo under /mnt/SDCARD/.media/splash_logo.png, use that instead
if [ -f "$SDCARD_PATH/.media/splash_logo.png" ]; then
	LOGO_PATH="$SDCARD_PATH/.media/splash_logo.png"
fi

if [ "$SHOW_SPLASH" = "yes" ] ; then
	cd $(dirname "$0")/$PLATFORM

	# we might overwrite this by installing MinUI,launch show2.elf from tmp
	cp show2.elf /tmp/show2.elf

	/tmp/show2.elf --mode=daemon --image="$LOGO_PATH" --text="Installing..." --logoheight=80 --progress=-1 &
	#sleep 0.5
	#SHOW_PID=$!
fi

CPU_PATH=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo performance > "$CPU_PATH"

# generic NextUI package install
for pakz in $PAKZ_PATH; do
	if [ ! -e "$pakz" ]; then continue; fi
	# leave packages meant for other platforms on the card
	case "$pakz" in *.tg5040.pakz|*.tg5050.pakz) continue ;; esac
	echo "TEXT:Extracting $pakz" > /tmp/show2.fifo
	cd $(dirname "$0")/$PLATFORM

	unzip -o -d "$SDCARD_PATH" "$pakz" # >> $pakz.txt
	rm -f "$pakz"

	# run postinstall if present
	if [ -f $SDCARD_PATH/post_install.sh ]; then
		echo "TEXT:Installing $pakz" > /tmp/show2.fifo
		$SDCARD_PATH/post_install.sh # > $pakz_post.txt
		rm -f $SDCARD_PATH/post_install.sh
	fi
done

# install/update
if [ -f "$UPDATE_PATH" ]; then 
	cd $(dirname "$0")/$PLATFORM
	if [ -d "$SYSTEM_PATH" ]; then
		echo "TEXT:Updating NextUI" > /tmp/show2.fifo
	else
		echo "TEXT:Installing NextUI" > /tmp/show2.fifo
	fi

	# clean replacement for core paths
	rm -rf $SYSTEM_PATH/$PLATFORM/bin
	rm -rf $SYSTEM_PATH/$PLATFORM/lib
	rm -rf $SYSTEM_PATH/$PLATFORM/paks/MinUI.pak

	unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" # &> /mnt/SDCARD/unzip.txt
	rm -f "$UPDATE_PATH"

	# the updated system finishes the install/update
	if [ -f $SYSTEM_PATH/$PLATFORM/bin/install.sh ]; then
		$SYSTEM_PATH/$PLATFORM/bin/install.sh # &> $SDCARD_PATH/log.txt
	fi
fi

#kill $SHOW_PID

LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
if [ -f "$LAUNCH_PATH" ] ; then
	"$LAUNCH_PATH"
fi

poweroff # under no circumstances should stock be allowed to touch this card
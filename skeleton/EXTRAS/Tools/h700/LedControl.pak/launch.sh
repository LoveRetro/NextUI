#!/bin/sh

cd "$(dirname "$0")"

# Seed the defaults once; after that the file belongs to the user.
TARGET="/mnt/SDCARD/.userdata/shared/ledsettings_h700.txt"
if [ ! -f "$TARGET" ]; then
	cp ./ledsettings_h700.txt "$TARGET"
fi

./ledcontrol.elf > ./log.txt 2>&1

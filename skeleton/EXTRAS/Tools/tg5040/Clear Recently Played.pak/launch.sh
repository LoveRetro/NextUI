#!/bin/sh

RECENT_FILE="$SDCARD_PATH/.userdata/shared/.minui/recent.txt"

if [ -f "$RECENT_FILE" ]; then
	rm "$RECENT_FILE"
fi

show2.elf --mode=simple --image "$SDCARD_PATH/.system/res/logo.png" --text="Recently Played cleared" --timeout=2

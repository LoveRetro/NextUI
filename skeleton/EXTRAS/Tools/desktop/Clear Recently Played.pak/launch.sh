#!/bin/sh

RECENT_FILE="$SDCARD_PATH/.userdata/shared/.minui/recent.txt"

if [ -f "$RECENT_FILE" ]; then
	rm "$RECENT_FILE"
fi

#!/bin/sh

TF1_PATH=/mnt/mmc
TF2_PATH=/mnt/sdcard
COMPAT_PATH=/mnt/SDCARD
PLATFORM=h700
SYSTEM_PATH="$TF2_PATH/.system/$PLATFORM"
UPDATE_PATH="$TF2_PATH/MinUI.zip"
LOG_PATH=/tmp/nextui-h700.log

trap '' USR1

if mountpoint -q "$TF1_PATH"; then
	LOG_PATH="$TF1_PATH/nextui-h700.log"
fi
: > "$LOG_PATH" 2>/dev/null || true

raw_log() {
	echo "$@" >> "$LOG_PATH" 2>/dev/null || true
}

has_pakz() {
	for pakz in "$TF2_PATH"/*.pakz; do
		[ -e "$pakz" ] && return 0
	done
	return 1
}

extract_payload() {
	if [ -x /tmp/nextui-h700-unzip ] && [ -x /tmp/nextui-h700-fbsplash ] && [ -f /tmp/nextui-h700/shim-common.sh ]; then
		return 0
	fi

	LINE=$(($(grep -na '^BINARY' "$0" | cut -d ':' -f 1 | head -1) + 1))
	tail -n +"$LINE" "$0" > /tmp/nextui-h700-data.tar.gz
	mkdir -p /tmp/nextui-h700
	if ! tar -xzf /tmp/nextui-h700-data.tar.gz -C /tmp/nextui-h700 >> "$LOG_PATH" 2>&1; then
		raw_log "embedded payload extraction failed"
		return 1
	fi
	cp /tmp/nextui-h700/unzip /tmp/nextui-h700-unzip
	chmod +x /tmp/nextui-h700-unzip
	if [ -x /tmp/nextui-h700/fbsplash ]; then
		cp /tmp/nextui-h700/fbsplash /tmp/nextui-h700-fbsplash
		chmod +x /tmp/nextui-h700-fbsplash
	fi
}

load_common() {
	extract_payload || return 1
	if [ ! -f /tmp/nextui-h700/shim-common.sh ]; then
		raw_log "embedded common helper missing"
		return 1
	fi
	SHIM_LOG_PATH="$LOG_PATH"
	SHIM_LOG_PREFIX=""
	SHIM_LOG_DATES=0
	. /tmp/nextui-h700/shim-common.sh
}

show_splash() {
	extract_payload || return 0
	[ -x /tmp/nextui-h700-fbsplash ] || return 0
	/tmp/nextui-h700-fbsplash "$1" >/dev/null 2>&1 || true
}

find_unzip() {
	for unzip_path in /usr/bin/unzip /bin/unzip /mnt/vendor/bin/unzip /tmp/nextui-h700-unzip; do
		if [ -x "$unzip_path" ]; then
			echo "$unzip_path"
			return 0
		fi
	done

	if extract_payload; then
		echo /tmp/nextui-h700-unzip
		return 0
	fi

	return 1
}

fallback_stock() {
	raw_log "falling back to stock frontend"
	if [ -x /mnt/vendor/bin/dmenu.bin ]; then
		exec /mnt/vendor/bin/dmenu.bin
	fi
	exit 1
}

if ! load_common; then
	show_splash "NEXTUI INSTALL MISSING"
	fallback_stock
fi

if [ -f /mnt/vendor/muos1.ini ] || [ -f /mnt/vendor/muos2.ini ]; then
	shim_log "stockmod muOS override is present; NextUI requires the stock boot target"
	show_splash "STOCK TARGET REQUIRED"
fi

if ! mount_tf2 "$TF2_PATH"; then
	repair_tf2
fi

if ! mount_tf2 "$TF2_PATH"; then
	shim_log "TF2 mount failed"
	show_splash "INSERT NEXTUI TF2 CARD"
	fallback_stock
fi

ensure_compat_path "$TF2_PATH" "$COMPAT_PATH"

if [ -f "$UPDATE_PATH" ]; then
	shim_log "install/update zip detected"
	show_splash "INSTALLING NEXTUI"
	if [ ! -x "$TF2_PATH/.tmp_update/$PLATFORM.sh" ]; then
		UNZIP_CMD=$(find_unzip)
		if [ -z "$UNZIP_CMD" ]; then
			shim_log "unzip helper unavailable"
			fallback_stock
		fi
		if ! "$UNZIP_CMD" -o "$UPDATE_PATH" ".tmp_update/*" -d "$TF2_PATH" >> "$LOG_PATH" 2>&1; then
			shim_log "updater bootstrap extraction failed"
			fallback_stock
		fi
	fi
fi

if [ -x "$TF2_PATH/.tmp_update/$PLATFORM.sh" ] && { [ -f "$UPDATE_PATH" ] || has_pakz; }; then
	show_splash "INSTALLING NEXTUI"
	"$TF2_PATH/.tmp_update/$PLATFORM.sh" >> "$LOG_PATH" 2>&1
elif [ -x "$SYSTEM_PATH/bin/install.sh" ] && [ -f "$UPDATE_PATH" ]; then
	show_splash "UPDATING NEXTUI"
	"$SYSTEM_PATH/bin/install.sh" >> "$LOG_PATH" 2>&1
fi

if [ -x "$SYSTEM_PATH/paks/MinUI.pak/launch.sh" ]; then
	exec "$SYSTEM_PATH/paks/MinUI.pak/launch.sh"
fi

shim_log "NextUI launch script missing"
show_splash "NEXTUI INSTALL MISSING"
fallback_stock

#!/bin/sh
# NOTE: becomes .tmp_update/h700.sh

PLATFORM="h700"
SDCARD_PATH="/mnt/SDCARD"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
PAKZ_PATH="$SDCARD_PATH/*.pakz"
SYSTEM_PATH="$SDCARD_PATH/.system"
USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
LOGS_PATH="$USERDATA_PATH/logs"
INSTALL_LOG="$LOGS_PATH/install.txt"
SCRIPT_DIR="$(dirname "$0")"
INIT_PATH="$SCRIPT_DIR/$PLATFORM/h700-install.sh"

# H700 path compatibility and stock runtime setup.
if [ ! -f "$INIT_PATH" ]; then
	echo "install: missing $INIT_PATH $(date)" >> /tmp/nextui-h700-install.log 2>/dev/null || true
	exit 1
fi
. "$INIT_PATH"
h700_init_install

cd "$SCRIPT_DIR/$PLATFORM" || exit 1

mkdir -p "$LOGS_PATH"
echo "install: starting $(date)" > "$INSTALL_LOG"

# Installation splash
shim_log "Installing..."
if [ -x ./show2.elf ]; then
	./show2.elf --mode=daemon --image="logo.png" --text="Installing..." --logoheight=128 --progress=-1 >/dev/null 2>&1 &
fi

h700_prepare_install

# generic NextUI package install
for pakz in $PAKZ_PATH; do
	if [ ! -e "$pakz" ]; then continue; fi
	echo "TEXT:Extracting $pakz" > /tmp/show2.fifo 2>/dev/null || true
	shim_log "extracting $pakz"
	./unzip -o -d "$SDCARD_PATH" "$pakz" >> "$INSTALL_LOG" 2>&1
	unzip_status=$?
	shim_log "extracting $pakz exited $unzip_status"
	[ "$unzip_status" = "0" ] || exit "$unzip_status"
	rm -f "$pakz"

	# run postinstall if present
	if [ -f "$SDCARD_PATH/post_install.sh" ]; then
		echo "TEXT:Installing $pakz" > /tmp/show2.fifo 2>/dev/null || true
		shim_log "running post_install.sh for $pakz"
		sh "$SDCARD_PATH/post_install.sh" >> "$INSTALL_LOG" 2>&1
		post_status=$?
		shim_log "post_install.sh exited $post_status"
		[ "$post_status" = "0" ] || exit "$post_status"
		rm -f "$SDCARD_PATH/post_install.sh"
	fi
done

# install/update
if [ -f "$UPDATE_PATH" ]; then
	if [ -d "$SYSTEM_PATH/$PLATFORM" ]; then
		echo "TEXT:Updating NextUI" > /tmp/show2.fifo 2>/dev/null || true
	else
		echo "TEXT:Installing NextUI" > /tmp/show2.fifo 2>/dev/null || true
	fi

	# clean replacement for core paths
	rm -rf "$SYSTEM_PATH/$PLATFORM/bin"
	rm -rf "$SYSTEM_PATH/$PLATFORM/lib"
	rm -rf "$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak"

	shim_log "extracting $UPDATE_PATH"
	./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" >> "$INSTALL_LOG" 2>&1
	unzip_status=$?
	shim_log "extracting $UPDATE_PATH exited $unzip_status"
	[ "$unzip_status" = "0" ] || exit "$unzip_status"
	rm -f "$UPDATE_PATH"
fi

# the installed system finishes setup (also needed after standalone pakz installs)
if [ -x "$SYSTEM_PATH/$PLATFORM/bin/install.sh" ]; then
	shim_log "running platform install.sh"
	"$SYSTEM_PATH/$PLATFORM/bin/install.sh" >> "$INSTALL_LOG" 2>&1
	install_status=$?
	shim_log "platform install.sh exited $install_status"
	[ "$install_status" = "0" ] || exit "$install_status"
fi

# launch NextUI
LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
if [ -x "$LAUNCH_PATH" ]; then
	shim_log "exec $LAUNCH_PATH"
	exec "$LAUNCH_PATH"
fi

shim_log "missing launch path; powering off"
poweroff

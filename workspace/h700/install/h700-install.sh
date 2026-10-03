#!/bin/sh
# Sourced by .tmp_update/h700.sh before the installed system is available.
# Keep this in the update payload; a fresh install cannot load SYSTEM_PATH helpers.

# Requires SCRIPT_DIR, PLATFORM, SDCARD_PATH, SYSTEM_PATH, and INSTALL_LOG.
h700_init_install() {
	COMMON_PATH="$SCRIPT_DIR/$PLATFORM/shim-common.sh"

	if [ ! -f "$COMMON_PATH" ]; then
		echo "install: missing $COMMON_PATH $(date)" >> /tmp/nextui-h700-install.log 2>/dev/null || true
		exit 1
	fi
	SHIM_LOG_PATH="$INSTALL_LOG"
	SHIM_LOG_PREFIX="install: "
	SHIM_LOG_DATES=1
	. "$COMMON_PATH"

	REAL_SDCARD_PATH="/mnt/sdcard"
	ensure_compat_path "$REAL_SDCARD_PATH" "$SDCARD_PATH"

	export LD_LIBRARY_PATH="$SYSTEM_PATH/$PLATFORM/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:$LD_LIBRARY_PATH"
	export PATH="$SYSTEM_PATH/$PLATFORM/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
	export PLATFORM
}

# Stop stock services that compete with the installer for display/network control.
h700_prepare_install() {
	sh "$SYSTEM_PATH/$PLATFORM/bin/governor.sh" performance 2>/dev/null || true
	systemctl stop NetworkManager 2>/dev/null || true
	killall brightCtrl.bin cexpert 2>/dev/null || true
}

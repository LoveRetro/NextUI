#!/bin/sh
# H700 stock-OS setup, sourced by MinUI.pak/launch.sh.
# Sourcing only defines functions; the launcher controls their execution order.

# Establish the stock SD-card mount and NextUI-compatible path before exporting paths.
h700_mount_sdcard() {
	REAL_SDCARD_PATH="/mnt/sdcard"
	COMPAT_SDCARD_PATH="/mnt/SDCARD"

	mkdir -p "$REAL_SDCARD_PATH"
	if ! mountpoint -q "$REAL_SDCARD_PATH"; then
		mount -t vfat -o rw,utf8,noatime /dev/mmcblk1p1 "$REAL_SDCARD_PATH" 2>/dev/null || true
	fi

	if ! mountpoint -q "$COMPAT_SDCARD_PATH" && [ ! -L "$COMPAT_SDCARD_PATH" ]; then
		if [ -e "$COMPAT_SDCARD_PATH" ]; then
			mount --bind "$REAL_SDCARD_PATH" "$COMPAT_SDCARD_PATH" 2>/dev/null || true
		else
			ln -s "$REAL_SDCARD_PATH" "$COMPAT_SDCARD_PATH" 2>/dev/null || true
		fi
	fi
}

# Requires SYSTEM_PATH. Configure the bundled runtime, SDL and locate stock graphics libraries.
h700_setup_runtime() {
	# Use the bundled CA certificates for curl's HTTPS verification.
	export CURL_CA_BUNDLE="$SYSTEM_PATH/etc/ssl/certs/ca-certificates.crt"
	# Select the stock OS graphics/audio backends explicitly.
	export SDL_VIDEODRIVER="mali"
	export SDL_AUDIODRIVER="alsa"
	# Stock OS has no udev service; disable SDL's udev-based joystick discovery.
	export SDL_JOYSTICK_DISABLE_UDEV=1
	export SDL_HIDAPI_JOYSTICK_DISABLE_UDEV=1

	for egl_path in /usr/lib/libEGL.so /usr/lib/libEGL.so.1 /usr/lib/libEGL.so.1.4.0 /usr/lib/aarch64-linux-gnu/libEGL.so /usr/lib/aarch64-linux-gnu/libEGL.so.1; do
		if [ -e "$egl_path" ]; then
			export SDL_VIDEO_EGL_DRIVER="$egl_path"
			break
		fi
	done
	for gles_path in /usr/lib/libGLESv2.so /usr/lib/libGLESv2.so.2 /usr/lib/libGLESv2.so.2.1.0 /usr/lib/aarch64-linux-gnu/libGLESv2.so /usr/lib/aarch64-linux-gnu/libGLESv2.so.2; do
		if [ -e "$gles_path" ]; then
			export SDL_VIDEO_GL_DRIVER="$gles_path"
			break
		fi
	done
}

# Requires device detection and an initialized LAUNCH_LOG.
log_runtime_state() {
	echo "launch: runtime state $(date)" >> "$LAUNCH_LOG"
	echo "launch: DEVICE=$DEVICE RGXX_MODEL=$RGXX_MODEL" >> "$LAUNCH_LOG"
	echo "launch: LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >> "$LAUNCH_LOG"
	ldd "$SYSTEM_PATH/bin/nextui.elf" >> "$LAUNCH_LOG" 2>&1 || true
	ldd "$SYSTEM_PATH/lib/libSDL2-2.0.so.0" >> "$LAUNCH_LOG" 2>&1 || true
	ls -l /usr/lib/libEGL.so* /usr/lib/libGLESv2.so* /usr/lib/aarch64-linux-gnu/libEGL.so* /usr/lib/aarch64-linux-gnu/libGLESv2.so* >> "$LAUNCH_LOG" 2>&1 || true
}

# Start the stock system bus if needed, immediately before audiomon.
ensure_system_dbus() {
	[ -S /run/dbus/system_bus_socket ] && return 0
	command -v dbus-daemon >/dev/null 2>&1 || return 1

	mkdir -p /run/dbus
	if [ ! -s /run/machine-id ] && command -v dbus-uuidgen >/dev/null 2>&1; then
		dbus-uuidgen > /run/machine-id 2>/dev/null || true
	fi
	dbus-daemon --system >/dev/null 2>&1 || true

	dbus_tries=0
	while [ ! -S /run/dbus/system_bus_socket ] && [ "$dbus_tries" -lt 20 ]; do
		sleep 0.1 2>/dev/null || sleep 1
		dbus_tries=$((dbus_tries + 1))
	done
	[ -S /run/dbus/system_bus_socket ]
}

# Requires exported paths and log directories. Hand control over from stock OS services.
h700_prepare_stock_os() {
	if [ -f /mnt/vendor/muos1.ini ] || [ -f /mnt/vendor/muos2.ini ]; then
		echo "stockmod muOS override files are present; stock boot target must be selected for TF1 dmenu.bin autoboot." > "$LOGS_PATH/stockmod-warning.txt"
	fi

	killall brightCtrl.bin cexpert 2>/dev/null || true
	systemctl stop NetworkManager 2>/dev/null || true
	if loginctl show-logind >/dev/null 2>&1; then
		mkdir -p /run/systemd/logind.conf.d /run/systemd/system 2>/dev/null || true
		cat > /run/systemd/logind.conf.d/nextui-h700.conf << EOF
[Login]
HandlePowerKey=ignore
HandlePowerKeyLongPress=ignore
EOF
		systemctl restart systemd-logind 2>/dev/null || true
	fi

	if [ -f "$SYSTEM_PATH/dat/dmenu.bin" ] && mountpoint -q /mnt/mmc; then
		if ! cmp -s "$SYSTEM_PATH/dat/dmenu.bin" /mnt/mmc/dmenu.bin 2>/dev/null; then
			cp "$SYSTEM_PATH/dat/dmenu.bin" /mnt/mmc/dmenu.bin 2>/dev/null || true
			sync
		fi
	fi

}

# Keep the power LED lit when NextUI takes over from the stock OS.
h700_init_leds() {
	echo 1 > /sys/class/power_supply/axp2202-battery/work_led 2>/dev/null || true
}

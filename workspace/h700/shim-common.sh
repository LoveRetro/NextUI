#!/bin/sh

: "${SHIM_LOG_PATH:=/tmp/nextui-h700.log}"
: "${SHIM_LOG_PREFIX:=}"
: "${SHIM_LOG_DATES:=0}"

shim_log() {
	if [ "$SHIM_LOG_DATES" = "1" ]; then
		echo "${SHIM_LOG_PREFIX}$* $(date)" >> "$SHIM_LOG_PATH" 2>/dev/null || true
	else
		echo "${SHIM_LOG_PREFIX}$*" >> "$SHIM_LOG_PATH" 2>/dev/null || true
	fi
}

ensure_compat_path() {
	real_path="$1"
	compat_path="$2"

	if mountpoint -q "$compat_path"; then
		return 0
	fi
	if [ -L "$compat_path" ]; then
		return 0
	fi
	if [ -e "$compat_path" ]; then
		mount --bind "$real_path" "$compat_path" 2>/dev/null || true
	else
		ln -s "$real_path" "$compat_path" 2>/dev/null || true
	fi
}

mount_tf2() {
	tf2_path="$1"
	tf2_device="${2:-/dev/mmcblk1p1}"

	mkdir -p "$tf2_path"
	if mountpoint -q "$tf2_path"; then
		return 0
	fi
	mount -t vfat -o rw,utf8,noatime "$tf2_device" "$tf2_path" 2>/dev/null ||
		mount -t exfat -o rw,noatime "$tf2_device" "$tf2_path" 2>/dev/null ||
		mount -o rw,noatime "$tf2_device" "$tf2_path" 2>/dev/null
}

repair_tf2() {
	tf2_device="${1:-/dev/mmcblk1p1}"
	fstype=$(blkid -o value -s TYPE "$tf2_device" 2>/dev/null)
	case "$fstype" in
		vfat|msdos|fat)
			command -v fsck.fat >/dev/null 2>&1 && fsck.fat -a "$tf2_device" >> "$SHIM_LOG_PATH" 2>&1 || true
			;;
		exfat)
			command -v fsck.exfat >/dev/null 2>&1 && fsck.exfat -a "$tf2_device" >> "$SHIM_LOG_PATH" 2>&1 || true
			;;
	esac
}

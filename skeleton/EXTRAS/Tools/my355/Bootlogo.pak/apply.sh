#!/bin/sh
# Boot logo installer for the Miyoo Flip (my355).
#
# Rewrites the boot image on raw NAND. That image also carries rk-kernel.dtb, so
# a bad or interrupted flash can leave the device booting without working
# hardware, or not booting at all. Nothing is written until the rebuilt image has
# been validated, so aborting before the flash leaves the device as it was.

set -x
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"
USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/my355}"
WORK="/tmp/bootlogo"
BACKUP_DIR="$USERDATA_PATH/bootlogo"
BACKUP="$BACKUP_DIR/boot.img.orig"
MIN_BATTERY=30

SHOW_PID=""
HEARTBEAT_PID=""

###############################

# A write to a fifo with no reader blocks forever, so check the daemon is alive.
notify() {
	[ -n "$SHOW_PID" ] || return 0
	kill -0 "$SHOW_PID" 2>/dev/null || return 0
	[ -p /tmp/show2.fifo ] || return 0
	echo "$1" > /tmp/show2.fifo 2>/dev/null
	return 0
}

step() {
	notify "PROGRESS:$1"
	notify "TEXT:$2"
	echo "== $2"
}

stop_heartbeat() {
	[ -n "$HEARTBEAT_PID" ] && kill "$HEARTBEAT_PID" 2>/dev/null
	HEARTBEAT_PID=""
	return 0
}

fail() {
	stop_heartbeat
	echo "ERROR: $1"
	notify "TEXT:$1"
	sleep 8
	notify "QUIT"
	exit 1
}

# So a slow flash is not mistaken for a hung one.
heartbeat() {
	elapsed=0
	while true; do
		sleep 5
		elapsed=$((elapsed + 5))
		notify "TEXT:Flashing - DO NOT POWER OFF (${elapsed}s)"
	done
}

###############################

LOGO_SPLASH="$SDCARD_PATH/.system/res/logo.png"
if [ -f "$SDCARD_PATH/.media/splash_logo.png" ]; then
	LOGO_SPLASH="$SDCARD_PATH/.media/splash_logo.png"
fi

show2.elf --mode=daemon --image="$LOGO_SPLASH" --text="Preparing..." --logoheight=80 --progress=-1 &
SHOW_PID=$!

###############################

step 5 "Checking device"

# Resolve by name: mtd1 is uboot and mtd3 is rootfs, so a wrong index is fatal.
MTD_LINE=$(grep '"boot"$' /proc/mtd 2>/dev/null)
[ -n "$MTD_LINE" ] || fail "No 'boot' partition found in /proc/mtd"

MTD_NAME=$(echo "$MTD_LINE" | cut -d: -f1)
MTD_DEV="/dev/$MTD_NAME"
MTD_RO="/dev/${MTD_NAME}ro"
MTD_SIZE=$(printf %d "0x$(echo "$MTD_LINE" | awk '{print $2}')" 2>/dev/null)

[ -c "$MTD_DEV" ] || fail "$MTD_DEV is missing"
[ -c "$MTD_RO" ] || fail "$MTD_RO is missing"
[ -n "$MTD_SIZE" ] && [ "$MTD_SIZE" -gt 0 ] || fail "Could not read boot partition size"

# A power loss mid-write bricks the device.
CAPACITY=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null || echo 0)
CHARGING=$(cat /sys/class/power_supply/ac/online 2>/dev/null || echo 0)
if [ "$CHARGING" != "1" ] && [ "$CAPACITY" -lt "$MIN_BATTERY" ]; then
	fail "Battery too low ($CAPACITY%) - charge to $MIN_BATTERY% or plug in"
fi

[ -f "$DIR/payload/logo.bmp" ] || fail "payload/logo.bmp is missing"

###############################

step 10 "Preparing environment"

# Leftovers from a previous run would be swept into the resource repack.
rm -rf "$WORK"
mkdir -p "$WORK" || fail "Could not create $WORK"
cp -r "$DIR"/payload/* "$WORK"/ || fail "Could not copy payload to $WORK"
cd "$WORK" || fail "Could not enter $WORK"

export PATH="$WORK/bin:$PATH"
export LD_LIBRARY_PATH="$WORK/lib:${LD_LIBRARY_PATH:-}"

###############################

step 15 "Backing up boot partition"

mkdir -p "$BACKUP_DIR" || fail "Could not create $BACKUP_DIR"

# Keep the first backup, or a re-run would overwrite the pristine image.
if [ -f "$BACKUP" ]; then
	echo "backup already present, keeping $BACKUP"
else
	NEED_KB=$((MTD_SIZE / 1024 + 1024))
	AVAIL_KB=$(df "$SDCARD_PATH" | awk 'NR==2 {print $4}')
	[ -n "$AVAIL_KB" ] || fail "Could not check free space on the SD card"
	[ "$AVAIL_KB" -ge "$NEED_KB" ] || fail "Not enough free space for a backup (need ${NEED_KB}KB)"

	dd if="$MTD_RO" of="$BACKUP.tmp" bs=131072 || fail "Could not read the boot partition"
	sync
	BACKUP_SIZE=$(wc -c < "$BACKUP.tmp")
	[ "$BACKUP_SIZE" -eq "$MTD_SIZE" ] || fail "Backup is incomplete ($BACKUP_SIZE of $MTD_SIZE bytes)"
	[ "$(head -c 8 "$BACKUP.tmp")" = "ANDROID!" ] || fail "Backup does not look like a boot image"
	mv "$BACKUP.tmp" "$BACKUP" || fail "Could not store the backup"
	sync
fi

###############################

step 25 "Extracting boot.img"

dd if="$MTD_RO" of=boot.img bs=131072 || fail "Could not read the boot partition"
[ -s boot.img ] || fail "Extracted boot.img is empty"
[ "$(head -c 8 boot.img)" = "ANDROID!" ] || fail "Extracted boot.img has no ANDROID! header"

###############################

step 40 "Unpacking boot.img"

mkdir -p bootimg || fail "Could not create bootimg/"
unpackbootimg -i boot.img -o bootimg || fail "Could not unpack boot.img"
[ -s bootimg/boot.img-kernel ] || fail "Unpacked kernel is missing or empty"
[ -s bootimg/boot.img-second ] || fail "Unpacked resource image is missing or empty"

###############################

step 50 "Unpacking resources"

mkdir -p bootres || fail "Could not create bootres/"
cp bootimg/boot.img-second bootres/ || fail "Could not stage the resource image"
cd bootres || fail "Could not enter bootres/"
rsce_tool -u boot.img-second || fail "Could not unpack the resource image"

# The resource image carries the device tree; never repack a partial unpack.
[ -s rk-kernel.dtb ] || fail "Device tree missing after unpack - aborting"
[ -s logo.bmp ] || fail "logo.bmp missing after unpack - aborting"

###############################

step 60 "Replacing logo"

cp -f "$WORK/logo.bmp" ./logo.bmp || fail "Could not replace logo.bmp"
cp -f "$WORK/logo.bmp" ./logo_kernel.bmp || fail "Could not replace logo_kernel.bmp"

###############################

step 70 "Packing updated resources"

# Clear positional params - they are reused to build the rsce_tool arguments.
set --
for file in *; do
	[ "$(basename "$file")" != "boot.img-second" ] && set -- "$@" -p "$file"
done
rsce_tool "$@" || fail "Could not repack the resource image"
[ -s boot-second ] || fail "Repacked resource image is missing or empty"

###############################

step 80 "Packing updated boot.img"

cp -f boot-second ../bootimg || fail "Could not stage the repacked resource image"
cd "$WORK" || fail "Could not return to $WORK"
rm -f boot.img

mkbootimg --kernel bootimg/boot.img-kernel --second bootimg/boot-second --base 0x10000000 --kernel_offset 0x00008000 --ramdisk_offset 0xf0000000 --second_offset 0x00f00000 --pagesize 2048 --hashtype sha1 -o boot.img || fail "Could not build the new boot.img"

# Last gate before anything is written to flash.
[ -s boot.img ] || fail "New boot.img is empty"
[ "$(head -c 8 boot.img)" = "ANDROID!" ] || fail "New boot.img has no ANDROID! header"

NEW_SIZE=$(wc -c < boot.img)
KERNEL_SIZE=$(wc -c < bootimg/boot.img-kernel)
[ "$NEW_SIZE" -gt "$KERNEL_SIZE" ] || fail "New boot.img ($NEW_SIZE) is smaller than its kernel - aborting"
[ "$NEW_SIZE" -le "$MTD_SIZE" ] || fail "New boot.img ($NEW_SIZE) exceeds the boot partition ($MTD_SIZE) - use a smaller logo"

###############################

step 90 "Flashing boot.img"
notify "TEXT:Flashing - DO NOT POWER OFF"

heartbeat &
HEARTBEAT_PID=$!

flashcp -v boot.img "$MTD_DEV"
FLASH_RC=$?
stop_heartbeat

# Partition may be partially written - do not reboot, leave the device reachable.
if [ $FLASH_RC -ne 0 ]; then
	fail "Flash FAILED (code $FLASH_RC). Do NOT power off. Backup: $BACKUP"
fi

sync

###############################

step 100 "Rebooting"
sleep 2
reboot

exit 0

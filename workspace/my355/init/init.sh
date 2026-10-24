#!/bin/sh
# will be copied to /miyoo355/app/355/ during build

set -x

DIR="$(cd "$(dirname "$0")" && pwd)"

# payload is shared with spruceOS, only ever replace an installed hook with a newer PAYLOAD_VERSION
if [ -f /mnt/SDCARD/force_hook_reinstall ]; then
	echo "force reinstall requested, proceeding with installation"
	rm -f /mnt/SDCARD/force_hook_reinstall
elif [ -f /usr/miyoo/bin/runmiyoo-original.sh ]; then
	NEW_VERSION=$(grep PAYLOAD_VERSION "$DIR/payload/runmiyoo.sh" | awk '{print $3}')
	OLD_VERSION=$(grep PAYLOAD_VERSION /usr/miyoo/bin/runmiyoo.sh | awk '{print $3}')
	# unversioned hooks (older NextUI) still work, leave them alone
	if [ -z "$OLD_VERSION" ] || [ "$NEW_VERSION" -le "$OLD_VERSION" ]; then
		echo "already installed"
		exit 0
	fi
fi

export PATH=/tmp/bin:$DIR/payload/bin:$PATH
export LD_LIBRARY_PATH=/tmp/lib:$DIR/payload/lib:$LD_LIBRARY_PATH

# For the "Loading" screen
touch /tmp/fbdisplay_exit
cat /dev/zero > /dev/fb0

# If we are not on firmware 2025+, prompt the user to update.
VERSION=$(cat /usr/miyoo/version)
YEAR=${VERSION:0:4}
if [ "$YEAR" -lt "2025" ]; then
	show2.elf --mode=simple --image="$DIR/res/logo.png" --text="NextUI requires firmware version 2025 or higher." --logoheight=80 --timeout=60
	poweroff
	while :; do
		sleep 1
	done
fi

# If we are on the left SD card slot, alert the user to move to the right slot (which mounts to /mnt/SDCARD)
tf1_mounted=`cat /proc/mounts | grep mnt/sdcard`
if [ "$tf1_mounted" == "" ]; then
	show2.elf --mode=simple --image="$DIR/res/logo.png" --text="Please use the right SD slot for NextUI." --logoheight=80 --timeout=60
	poweroff
	while :; do
		sleep 1
	done
fi

show2.elf --mode=daemon --image="$DIR/res/logo.png" --text="Preparing environment..." --logoheight=80 &
echo "preparing environment"
cd "$DIR"
cp -r payload/* /tmp
cd /tmp

echo "TEXT:Extracting rootfs" > /tmp/show2.fifo
echo "PROGRESS:20" > /tmp/show2.fifo
echo "extracting rootfs"
dd if=/dev/mtd3ro of=old_rootfs.squashfs bs=131072

echo "TEXT:Unpacking rootfs" > /tmp/show2.fifo
echo "PROGRESS:40" > /tmp/show2.fifo
echo "unpacking rootfs"
unsquashfs old_rootfs.squashfs

echo "TEXT:Injecting hook" > /tmp/show2.fifo
echo "PROGRESS:60" > /tmp/show2.fifo
echo "swapping runmiyoo.sh"
if [ ! -f squashfs-root/usr/miyoo/bin/runmiyoo-original.sh ]; then
	mv squashfs-root/usr/miyoo/bin/runmiyoo.sh squashfs-root/usr/miyoo/bin/runmiyoo-original.sh
fi
mv runmiyoo.sh squashfs-root/usr/miyoo/bin/

echo "TEXT:Packing rootfs" > /tmp/show2.fifo
echo "PROGRESS:80" > /tmp/show2.fifo
echo "packing updated rootfs"
mksquashfs squashfs-root new_rootfs.squashfs -comp gzip -b 131072 -noappend -exports -all-root -force-uid 0 -force-gid 0

echo "TEXT:Flashing rootfs" > /tmp/show2.fifo
echo "flashing updated rootfs"
# mount so reboot remains available
mkdir -p /tmp/rootfs
mount /tmp/new_rootfs.squashfs /tmp/rootfs
export PATH=/tmp/rootfs/bin:/tmp/rootfs/usr/bin:/tmp/rootfs/sbin:$PATH
export LD_LIBRARY_PATH=/tmp/rootfs/lib:/tmp/rootfs/usr/lib:$LD_LIBRARY_PATH
flashcp new_rootfs.squashfs /dev/mtd3 && sync

echo "TEXT:Rebooting" > /tmp/show2.fifo
echo "PROGRESS:100" > /tmp/show2.fifo
echo "done, rebooting"
sleep 2
reboot
while :; do
	sleep 1
done
exit

#!/bin/sh
# will be copied to /miyoo355/app/355/ during build

set -x

if [ -f /usr/miyoo/bin/runmiyoo-original.sh ]; then
	echo "already installed"
	exit 0
fi

DIR="$(cd "$(dirname "$0")" && pwd)"

export PATH=/tmp/bin:$DIR/payload/bin:$PATH
export LD_LIBRARY_PATH=/tmp/lib:$DIR/payload/lib:$LD_LIBRARY_PATH

# For the "Loading" screen
touch /tmp/fbdisplay_exit
./show2.elf --mode=daemon --image="$DIR/res/logo.png" --text="Preparing environment..." --logoheight=128 --progress=-1 &
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
mv squashfs-root/usr/miyoo/bin/runmiyoo.sh squashfs-root/usr/miyoo/bin/runmiyoo-original.sh
mv runmiyoo.sh squashfs-root/usr/miyoo/bin/

echo "TEXT:Packing rootfs" > /tmp/show2.fifo
echo "PROGRESS:80" > /tmp/show2.fifo
echo "packing updated rootfs"
mksquashfs squashfs-root new_rootfs.squashfs -comp gzip -b 131072 -noappend -exports -all-root -force-uid 0 -force-gid 0

# mount so reboot remains available
mkdir -p /tmp/rootfs
mount /tmp/new_rootfs.squashfs /tmp/rootfs
export PATH=/tmp/rootfs/bin:/tmp/rootfs/usr/bin:/tmp/rootfs/sbin:$PATH
export LD_LIBRARY_PATH=/tmp/rootfs/lib:/tmp/rootfs/usr/lib:$LD_LIBRARY_PATH

echo "TEXT:Flashing rootfs" > /tmp/show2.fifo
echo "flashing updated rootfs"
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

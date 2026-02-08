#!/bin/sh
set -x

export PATH=/tmp/bin:$PATH
export LD_LIBRARY_PATH=/tmp/lib:$LD_LIBRARY_PATH

DIR="$(cd "$(dirname "$0")" && pwd)"
LOGO_PATH="$SYSTEM_PATH/res/logo.png"
# If the user put a custom logo under /mnt/SDCARD/.media/splash_logo.png, use that instead
if [ -f "$SDCARD_PATH/.media/splash_logo.png" ]; then
	LOGO_PATH="$SDCARD_PATH/.media/splash_logo.png"
fi

show2.elf --mode=daemon --image="$LOGO_PATH" --text="Preparing environment..." --logoheight=80 --progress=-1 &
echo "preparing environment"
cd $(dirname "$0")
cp -r payload/* /tmp
cd /tmp

echo "TEXT:Extracting boot.img" > /tmp/show2.fifo
echo "PROGRESS:20" > /tmp/show2.fifo
echo "extracting boot.img"
dd if=/dev/mtd2ro of=boot.img bs=131072

echo "TEXT:Unpacking boot.img" > /tmp/show2.fifo
echo "PROGRESS:40" > /tmp/show2.fifo
echo "unpacking boot.img"
mkdir -p bootimg
unpackbootimg -i boot.img -o bootimg

echo "TEXT:Unpacking resources" > /tmp/show2.fifo
echo "PROGRESS:50" > /tmp/show2.fifo
mkdir -p bootres
cp bootimg/boot.img-second bootres/
cd bootres
rsce_tool -u boot.img-second

echo "TEXT:Replacing logo" > /tmp/show2.fifo
echo "PROGRESS:60" > /tmp/show2.fifo
echo "replacing logo"
cp -f ../logo.bmp ./
cp -f ../logo.bmp ./logo_kernel.bmp

echo "TEXT:Packing updated resources" > /tmp/show2.fifo
echo "PROGRESS:70" > /tmp/show2.fifo
for file in *; do
    [ "$(basename "$file")" != "boot.img-second" ] && set -- "$@" -p "$file"
done
rsce_tool "$@"

echo "TEXT:Packing updated boot.img" > /tmp/show2.fifo
echo "PROGRESS:80" > /tmp/show2.fifo
echo "packing updated boot.img"
cp -f boot-second ../bootimg
cd ../
rm boot.img
mkbootimg --kernel bootimg/boot.img-kernel --second bootimg/boot-second --base 0x10000000 --kernel_offset 0x00008000 --ramdisk_offset 0xf0000000 --second_offset 0x00f00000 --pagesize 2048 --hashtype sha1 -o boot.img

echo "TEXT:Flashing boot.img" > /tmp/show2.fifo
echo "PROGRESS:90" > /tmp/show2.fifo
echo "flashing updated boot.img"
flashcp boot.img /dev/mtd2 && sync

echo "TEXT:Rebooting" > /tmp/show2.fifo
echo "PROGRESS:100" > /tmp/show2.fifo
echo "done, rebooting"
sleep 2

# self-destruct
#mv $DIR $DIR.disabled
reboot

exit
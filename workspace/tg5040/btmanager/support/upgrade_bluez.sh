#! /bin/sh

TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Smart Pro S" ]; then
	return 0
fi

# if /usr/lib/libsbc.so.1.3.1 exists, we are already updated
if [ -f "/usr/lib/libsbc.so.1.3.1" ]; then
    return 0
fi

BLUEZ_PATH=/mnt/SDCARD/.update_bluez
if [ -d "$BLUEZ_PATH" ]; then
	echo "Updating bluez..."
else
    echo "Unable to locate update files, exiting."
    return 0
fi

# this will become post_install.sh inside the pakz
rm -rf /mnt/SDCARD/btmgr_backup
mkdir -p /mnt/SDCARD/btmgr_backup/usr/bin
mkdir -p /mnt/SDCARD/btmgr_backup/usr/lib/alsa-lib
mkdir -p /mnt/SDCARD/btmgr_backup/usr/lib64/alsa-lib
mkdir -p /mnt/SDCARD/btmgr_backup/etc/dbus-1/system.d
mkdir -p /mnt/SDCARD/btmgr_backup/usr/share/alsa/alsa.conf.d/

# bluez
cp /usr/bin/bluetoothctl /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/btmon /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/rctest /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/l2test /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/l2ping /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/bluemoon /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/hex2hcd /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/mpris-proxy /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/btattach /mnt/SDCARD/btmgr_backup/usr/bin/
#cp /usr/bin/isotest /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/bluetoothd /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/obexd /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/lib/libbluetooth.so.3.19.2 /mnt/SDCARD/btmgr_backup/usr/lib/
cp /usr/lib64/libbluetooth.so.3.19.2 /mnt/SDCARD/btmgr_backup/usr/lib64/
cp /etc/dbus-1/system.d/bluetooth.conf /mnt/SDCARD/btmgr_backup/etc/dbus-1/system.d/
cp /etc/dbus-1/system.d/bluetooth-mesh.conf /mnt/SDCARD/btmgr_backup/etc/dbus-1/system.d/
cp /etc/dbus-1/system.d/bluetooth-mesh-adapter.conf /mnt/SDCARD/btmgr_backup/etc/dbus-1/system.d/

# bluealsa
cp /usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so /mnt/SDCARD/btmgr_backup/usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so
cp /usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so /mnt/SDCARD/btmgr_backup/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
cp /usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so /mnt/SDCARD/btmgr_backup/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so
cp /usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so /mnt/SDCARD/btmgr_backup/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so
cp /usr/bin/bluealsa /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/bluealsa-aplay /mnt/SDCARD/btmgr_backup/usr/bin/
# cp /etc/dbus-1/system.d/bluealsa.conf /mnt/SDCARD/btmgr_backup/etc/dbus-1/system.d/
cp /usr/share/alsa/alsa.conf.d/20-bluealsa.conf /mnt/SDCARD/btmgr_backup/usr/share/alsa/alsa.conf.d/

# sbc
cp /usr/lib/libsbc.so.1.2.1 /mnt/SDCARD/btmgr_backup/usr/lib/
cp /usr/lib64/libsbc.so.1.2.1 /mnt/SDCARD/btmgr_backup/usr/lib64/
cp /usr/bin/sbcinfo /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/sbcdec /mnt/SDCARD/btmgr_backup/usr/bin/
cp /usr/bin/sbcenc /mnt/SDCARD/btmgr_backup/usr/bin/

# compress backup and clean up
backupfile="/mnt/SDCARD/btmgr_$(date +%Y%m%d_%H%M%S).zip"
tar czf $backupfile /mnt/SDCARD/btmgr_backup/
rm -rf /mnt/SDCARD/btmgr_backup

# deploy update
cd $BLUEZ_PATH

# copy relative to root
#bluez
mv ./usr/bin/bluetoothctl /usr/bin/
mv ./usr/bin/btmon /usr/bin/
mv ./usr/bin/rctest /usr/bin/
mv ./usr/bin/l2test /usr/bin/
mv ./usr/bin/l2ping /usr/bin/
mv ./usr/bin/bluemoon /usr/bin/
mv ./usr/bin/hex2hcd /usr/bin/
mv ./usr/bin/mpris-proxy /usr/bin/
mv ./usr/bin/btattach /usr/bin/
mv ./usr/bin/isotest /usr/bin/
mv ./usr/bin/bluetoothd /usr/bin/
mv ./usr/bin/obexd /usr/bin/
mv ./usr/lib/libbluetooth.so.3.19.15 /usr/lib/
mv ./usr/lib64/libbluetooth.so.3.19.15 /usr/lib64/
#mv ./etc/dbus-1/system.d/bluetooth.conf /etc/dbus-1/system.d/
#mv ./etc/dbus-1/system.d/bluetooth-mesh.conf /etc/dbus-1/system.d/
#mv ./etc/dbus-1/system.d/bluetooth-mesh-adapter.conf /etc/dbus-1/system.d/

# bluealsa
mv ./usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so /usr/lib/alsa-lib/
mv ./usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so /usr/lib/alsa-lib/
mv ./usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so /usr/lib64/alsa-lib/
mv ./usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so /usr/lib64/alsa-lib/
mv ./usr/bin/bluealsa /usr/bin/
mv ./usr/bin/bluealsa-aplay /usr/bin/
mv ./etc/dbus-1/system.d/bluealsa.conf /etc/dbus-1/system.d/
mv ./usr/share/alsa/alsa.conf.d/20-bluealsa.conf /usr/share/alsa/alsa.conf.d/

# sbc
mv ./usr/lib/libsbc.so.1.3.1 /usr/lib/
mv ./usr/lib64/libsbc.so.1.3.1 /usr/lib64/
mv ./usr/bin/sbcinfo build/.update_bluez/usr/bin/
mv ./usr/bin/sbcdec build/.update_bluez/usr/bin/
mv ./usr/bin/sbcenc build/.update_bluez/usr/bin/

# symlinks
cd /usr/lib/ && ln -s -f libbluetooth.so.3.19.15 libbluetooth.so.3
cd /usr/lib/ && ln -s -f libbluetooth.so.3.19.15 libbluetooth.so
cd /usr/lib64/ && ln -s -f libbluetooth.so.3.19.15 libbluetooth.so.3
cd /usr/lib64/ && ln -s -f libbluetooth.so.3.19.15 libbluetooth.so
cd /usr/lib/ && ln -s -f libsbc.so.1.3.1 libsbc.so.1
cd /usr/lib/ && ln -s -f libsbc.so.1.3.1 libsbc.so
cd /usr/lib/64 && ln -s -f libsbc.so.1.3.1 libsbc.so.1
cd /usr/lib/64 && ln -s -f libsbc.so.1.3.1 libsbc.so

# clean up
rm -rf $BLUEZ_PATH

echo "Finished upgrading bluez."
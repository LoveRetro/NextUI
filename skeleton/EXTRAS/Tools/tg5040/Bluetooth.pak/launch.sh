#!/bin/sh

SDCARD_PATH="/mnt/SDCARD"
USERDATA_PATH="$SDCARD_PATH/.userdata"


#!/bin/bash

# Check if bluetoothd is running
if pgrep -x "bluetoothd" > /dev/null; then
    echo "Bluetooth daemon is already running."
else
    echo "Starting Bluetooth daemon..."
    /etc/bluetooth/bluetoothd &
fi


cd $(dirname "$0")


./bluetooth.elf > $USERDATA_PATH/tg5040/logs/bluetooth.log 2>&1

#!/bin/sh

# TODO: This is launched from minui, I dont think we need to set the paths again
PLATFORM="v10"
SDCARD_PATH="/storage/TF2"
USERDATA_PATH=USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"

LEGACY_TARGET_PATH="$USERDATA_PATH/minuisettings.txt"
TARGET_PATH="$SHARED_USERDATA_PATH/minuisettings.txt"

cd $(dirname "$0")
if [ -f "$LEGACY_TARGET_PATH" ]; then
    mv "$LEGACY_TARGET_PATH" "$TARGET_PATH" >> launch.log
    echo "File migrated to $SHARED_USERDATA_PATH" >> launch.log
elif [ ! -f "$TARGET_PATH" ]; then
    cp ./minuisettings.txt "$SHARED_USERDATA_PATH" >> launch.log
    echo "File copied to $SHARED_USERDATA_PATH" >> launch.log
else
    echo "File already exists in $SHARED_USERDATA_PATH" >> launch.log
fi

./settings.elf > settings.log 2>&1

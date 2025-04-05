#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

"$SCRIPT_PATH/compile_mac_os.sh" || exit 1

DEV_ENV_FILE="$SCRIPT_PATH/dev_env.sh"

if [ ! -f "$DEV_ENV_FILE" ]; then
    echo "FAKE_SD_PATH=/Users/XXX/MinUI-SD">"$DEV_ENV_FILE"
    echo "RETRO_ARCH_CORES=/Users/XXX/RetroArchCores">>"$DEV_ENV_FILE"
    echo "The file $SCRIPT_PATH is missing."
    echo "A template has been created, please edit the file with proper values."
    exit 1
fi

source "$DEV_ENV_FILE"

if [ ! -d "$FAKE_SD_PATH" ]; then
    echo "The FAKE_SD_PATH variable set in $DEV_ENV_FILE seems to be invalid"
    exit 1
fi
if [ ! -d "$RETRO_ARCH_CORES" ]; then
    echo "The RETRO_ARCH_CORES variable set in $DEV_ENV_FILE seems to be invalid"
    exit 1
fi

MINARCH_PATH="$SCRIPT_PATH/../build/macos/minarch"
if [ ! -f "$MINARCH_PATH" ]; then
    echo "Can't find minarch at $MINARCH_PATH"
    exit 1
fi

CORE="$1"
ROM="$2"

if [ ! -f "$RETRO_ARCH_CORES/$CORE" ]; then
    echo "Can't find core $CORE in $RETRO_ARCH_CORES"
    exit 1
fi

if [ ! -f "$FAKE_SD_PATH/$ROM" ]; then
    echo "Can't find rom $ROM in $FAKE_SD_PATH"
    exit 1
fi

LAUNCHER=""
if [ "$3" = "--debug" ]; then
    LAUNCHER=lldb
fi

$LAUNCHER "$MINARCH_PATH" "$RETRO_ARCH_CORES/$CORE" "$FAKE_SD_PATH/$ROM"

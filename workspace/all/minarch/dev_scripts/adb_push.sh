#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

adb push "$SCRIPT_PATH/../build/tg5040/minarch.elf" /mnt/SDCARD/.system/tg5040/bin/minarch.elf

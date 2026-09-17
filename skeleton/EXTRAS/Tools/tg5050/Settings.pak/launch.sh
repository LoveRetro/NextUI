#!/bin/sh

export SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}"
export USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/tg5050}"
export SHARED_USERDATA_PATH="${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}"

cd "$(dirname "$0")" || exit 1
./settings.elf > settings.log 2>&1

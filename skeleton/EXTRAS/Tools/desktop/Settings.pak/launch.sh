#!/bin/sh

export SDCARD_PATH="${SDCARD_PATH:-/var/tmp/nextui/sdcard}"
export USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/desktop}"
export SHARED_USERDATA_PATH="${SHARED_USERDATA_PATH:-$SDCARD_PATH/.userdata/shared}"

cd "$(dirname "$0")" || exit 1
./settings.elf > settings.log 2>&1

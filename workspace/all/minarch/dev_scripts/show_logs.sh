#!/bin/bash

function usage() {
    echo "Usage: $(basename "$0") [-f] MODULE"
    exit 1
}

CMD="cat"
while getopts "f" o; do
    case "$o" in
        f)
            CMD="tail -f"
            ;;
        *)
            break
            ;;
    esac
done

shift $((OPTIND-1))
MODULE="$1"

if [ -z "$MODULE" ]; then
    usage
fi

adb shell $CMD /mnt/SDCARD/.userdata/tg5040/logs/$MODULE.txt


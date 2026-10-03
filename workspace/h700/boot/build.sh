#!/bin/sh

SOURCE=boot.sh
TARGET=dmenu.bin

mkdir -p output
${CROSS_COMPILE}gcc -Os -s fbsplash.c -o ./output/fbsplash
cp ../other/unzip60/unzip ./output/
cp ../shim-common.sh ./output/

cd output || exit 1
tar -czf data fbsplash unzip shim-common.sh
cat ../$SOURCE > $TARGET
echo BINARY >> $TARGET
cat data >> $TARGET
chmod +x $TARGET

PAYLOAD_LINE=$(($(grep -na '^BINARY' "$TARGET" | cut -d: -f1 | head -1) + 1))
tail -n +"$PAYLOAD_LINE" "$TARGET" | gzip -t

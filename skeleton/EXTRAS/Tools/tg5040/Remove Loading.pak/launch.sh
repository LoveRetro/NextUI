#!/bin/sh

DIR="$(dirname "$0")"
cd "$DIR"

sed -i '/^\/usr\/sbin\/pic2fb \/etc\/splash.png/d' /etc/init.d/runtrimui
show2.elf --mode=simple --image "$SDCARD_PATH/.system/res/logo.png" --text="Done" --timeout=2

mv "$DIR" "$DIR.disabled"
#!/bin/sh

DIR="$(dirname "$0")"
cd "$DIR"

sed -i '/^\/usr\/trimui\/bin\/sdl2display \/usr\/trimui\/bin\/splash.png \&/d' /mnt/SDCARD/.tmp_update/tg5050.sh
show2.elf --mode=simple --image "$SDCARD_PATH/.system/res/logo.png" --text="Done" --timeout=2

mv "$DIR" "$DIR.disabled"
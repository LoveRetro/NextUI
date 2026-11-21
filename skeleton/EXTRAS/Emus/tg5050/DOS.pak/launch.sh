#!/bin/sh

PAK_DIR=$(dirname "$0")
EMU_TAG=$(basename "$PAK_DIR" .pak)

mkdir -p "$BIOS_PATH/$EMU_TAG" "$SAVES_PATH/$EMU_TAG" "$CHEATS_PATH/$EMU_TAG" || exit 1
export HOME="$USERDATA_PATH"
cd "$HOME" || exit 1

exec minarch.elf "$PAK_DIR/dosbox_pure_libretro.so" "$1" > "$LOGS_PATH/$EMU_TAG.txt" 2>&1

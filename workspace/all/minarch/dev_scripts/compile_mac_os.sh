#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

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

cd "$SCRIPT_PATH/.."

clang minarch.c \
    ../common/config.c \
    ../common/scaler.c \
    ../common/utils.c \
    ../common/api.c \
    ../libbatmondb/batmondb.c \
    ../../macos/platform/platform.c \
    \
    -o build/macos/minarch \
    \
    -O0 -g \
    \
    -std=gnu99 \
    -fomit-frame-pointer \
    -fno-common \
    \
    -Wno-tautological-constant-out-of-range-compare \
    -Wno-asm-operand-widths \
    -Wno-deprecated-declarations \
    \
    -DBUILD_DATE=\"$(date +%Y.%m.%d)\" \
    -DBUILD_HASH=\"$(cat ../../hash.txt)\" \
    -DPLATFORM=\"macos\" \
    -DSDCARD_PATH=\"$FAKE_SD_PATH\" \
    -DFAKE_PLATFORM \
    -DUSE_SDL2 \
    \
    -I. \
    -I../common/ \
    -I../../macos/platform/ \
    -I../libbatmondb/ \
    -I/opt/homebrew/include \
    -Ilibretro-common/include \
    \
    -L/opt/homebrew/lib \
    \
    -ldl \
    -lSDL2 \
    -lSDL2_image \
    -lSDL2_ttf \
    -lpthread \
    -lsqlite3 \
    -lsamplerate \
    -lm \
    $(xcode-select -p)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib/libz.tbd

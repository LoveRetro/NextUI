#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

"$SCRIPT_PATH/compile_and_launch.sh" "fbneo_libretro.dylib" "Roms/Arcade (FBN)/1943.zip" $*

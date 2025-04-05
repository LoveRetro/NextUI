#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

"$SCRIPT_PATH/compile_and_launch.sh" "cap32_libretro.dylib" "Roms/Amstrad (CPC)/batmanmv.dsk" $*

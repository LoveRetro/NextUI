#!/bin/bash

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

"$SCRIPT_PATH/compile_and_launch.sh" "gambatte_libretro.dylib" "Roms/Game Boy (GB)/Super Mario Land (W) (V1.1) [!].gb" $*

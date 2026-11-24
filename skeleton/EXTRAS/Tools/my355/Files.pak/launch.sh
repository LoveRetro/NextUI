#!/bin/sh

cd $(dirname "$0")

HOME="$SDCARD_PATH"
CFG="my355.cfg"


./NextCommander --config $CFG > launch.log
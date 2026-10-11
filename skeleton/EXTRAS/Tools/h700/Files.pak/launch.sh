#!/bin/sh

cd $(dirname "$0")

HOME="$SDCARD_PATH"
CFG="h700.cfg"

./NextCommander --config $CFG

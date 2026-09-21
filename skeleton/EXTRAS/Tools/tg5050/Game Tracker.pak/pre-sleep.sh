#!/bin/sh
# pre-sleep.sh -- close the open play_activity row before the device sleeps
# or powers off (fire-and-forget, cannot veto sleep).

gametimectl.elf stop_all

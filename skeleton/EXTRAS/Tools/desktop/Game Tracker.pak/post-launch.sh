#!/bin/sh
# post-launch.sh -- close the play_activity row for the rom that just exited.

[ "$HOOK_TYPE" = "rom" ] || exit 0

gametimectl.elf stop "$HOOK_ROM_PATH"

#!/bin/sh
# pre-launch.sh -- start tracking play time for the rom about to launch.
#
# Scanned by pak-hooks.sh alongside every other registered pak's pre-launch.sh
# in the same pass, with no ordering guarantee relative to any pak that might
# still veto the launch -- so this always starts tracking unconditionally.
# Any pak that vetoes a launch (exits non-zero from its own pre-launch.sh)
# is responsible for calling `gametimectl.elf stop_all` itself -- see the
# ordering note in HOOKS.md.

[ "$HOOK_TYPE" = "rom" ] || exit 0

gametimectl.elf start "$HOOK_ROM_PATH"

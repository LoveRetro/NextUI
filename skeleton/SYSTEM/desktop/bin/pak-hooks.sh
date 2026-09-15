#!/bin/sh
# pak-hooks.sh - self-registering, pak-scoped lifecycle hooks
#
# The sole hook mechanism in NextUI: scans installed Tools paks for a
# boot.sh / pre-launch.sh / post-launch.sh / pre-sleep.sh / post-resume.sh
# file sitting right next to their launch.sh. Presence of the file *is* the
# registration: no install step, no arming, and removing the pak removes
# its hooks with it.
#
# Usage: pak-hooks.sh rebuild-cache
#        pak-hooks.sh boot           (fire-and-forget, exit code ignored)
#        pak-hooks.sh pre-launch     (exit != 0 vetoes the launch)
#        pak-hooks.sh post-launch
#        pak-hooks.sh pre-sleep      (fire-and-forget, exit code ignored)
#        pak-hooks.sh post-resume    (fire-and-forget, exit code ignored)
#
# The list of qualifying paks is cached to avoid re-scanning every Tools pak
# on every single launch/sleep. The cache is rebuilt when returning to the
# main menu (see nextui.c), not at call time, so a freshly installed/removed
# pak is picked up on the very next visit to the menu instead of going stale
# until the next reboot. At boot, no menu visit has happened yet, so the
# first call below builds the cache fresh.

: "${SDCARD_PATH:=/var/tmp/nextui/sdcard}"
: "${PLATFORM:=desktop}"
CACHE="/tmp/pak_hooks_cache.txt"

rebuild_cache() {
	: > "$CACHE"
	for pak in "$SDCARD_PATH"/Tools/"$PLATFORM"/*.pak; do
		[ -d "$pak" ] || continue
		if [ -x "$pak/boot.sh" ] || [ -x "$pak/pre-launch.sh" ] \
			|| [ -x "$pak/post-launch.sh" ] || [ -x "$pak/pre-sleep.sh" ] \
			|| [ -x "$pak/post-resume.sh" ]; then
			echo "$pak" >> "$CACHE"
		fi
	done
}

run_phase() {
	PHASE="$1" # boot | pre-launch | post-launch | pre-sleep | post-resume
	SCRIPT_NAME="$PHASE.sh"
	# no HOOK_PHASE env var here: the script's own filename already tells it
	# which phase it's running for, so a separate variable would just repeat
	# that. Share logic across phases by sourcing a common file instead.

	[ -f "$CACHE" ] || rebuild_cache

	VETOED=0
	while IFS= read -r pak; do
		[ -n "$pak" ] || continue
		[ -x "$pak/$SCRIPT_NAME" ] || continue
		if ! "$pak/$SCRIPT_NAME"; then
			VETOED=1
		fi
	done < "$CACHE"

	# only pre-launch actually vetoes anything; every other phase is
	# fire-and-forget and callers don't check this exit code
	exit $VETOED
}

case "$1" in
	rebuild-cache) rebuild_cache ;;
	boot|pre-launch|post-launch|pre-sleep|post-resume) run_phase "$1" ;;
esac

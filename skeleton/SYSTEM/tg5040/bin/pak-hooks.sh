#!/bin/sh
# pak-hooks.sh - self-registering, pak-scoped launch hooks
#
# Unlike run_hooks.sh (scripts dropped into $USERDATA_PATH/.hooks/, which any
# pak has to arm/disarm itself), this scans installed Tools paks for a
# pre-launch.sh and/or post-launch.sh file sitting right next to their
# launch.sh. Presence of the file *is* the registration: no install step, and
# removing the pak removes its hook with it.
#
# Usage: pak-hooks.sh rebuild-cache
#        pak-hooks.sh pre-launch    (exit != 0 vetoes the launch)
#        pak-hooks.sh post-launch
#
# The list of qualifying paks is cached to avoid re-scanning every Tools pak
# on every single launch. The cache is rebuilt when returning to the main
# menu (see nextui.c), not at launch time, so a freshly installed/removed pak
# is picked up on the very next visit to the menu instead of going stale
# until the next reboot.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
CACHE="/tmp/pak_hooks_cache.txt"

rebuild_cache() {
	: > "$CACHE"
	for pak in "$SDCARD_PATH"/Tools/"$PLATFORM"/*.pak; do
		[ -d "$pak" ] || continue
		if [ -x "$pak/pre-launch.sh" ] || [ -x "$pak/post-launch.sh" ]; then
			echo "$pak" >> "$CACHE"
		fi
	done
}

run_phase() {
	PHASE="$1" # pre-launch | post-launch
	SCRIPT_NAME="$PHASE.sh"
	export HOOK_PHASE="${PHASE%-launch}"

	[ -f "$CACHE" ] || rebuild_cache

	VETOED=0
	while IFS= read -r pak; do
		[ -n "$pak" ] || continue
		[ -x "$pak/$SCRIPT_NAME" ] || continue
		if ! "$pak/$SCRIPT_NAME"; then
			VETOED=1
		fi
	done < "$CACHE"

	exit $VETOED
}

case "$1" in
	rebuild-cache) rebuild_cache ;;
	pre-launch|post-launch) run_phase "$1" ;;
esac

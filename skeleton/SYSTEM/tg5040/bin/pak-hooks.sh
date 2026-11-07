#!/bin/sh
# pak-hooks.sh - pak-scoped lifecycle hooks
#
# A Tools pak registers for a lifecycle event by shipping a script named after
# it next to its own launch.sh:
#
#   boot.sh  pre-launch.sh  post-launch.sh  pre-sleep.sh  post-resume.sh
#
# The file being there is the registration: no install step, and deleting the
# pak deletes its hooks. A pre-launch.sh that exits non-zero vetoes the launch.
#
# Not called by the launcher directly: run_hooks.sh calls it first for every
# event it runs, so the call sites stay upstream's. See HOOKS.md.
#
# Usage: pak-hooks.sh <event>    event is run_hooks.sh's dir name minus ".d"
#        pak-hooks.sh rebuild-cache

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
CACHE="/tmp/pak_hooks_cache.txt"

# Paks that ship at least one hook. /tmp is empty at boot, so the first event
# builds it; after that it is rebuilt only when a Tools pak exits (post-launch
# below), since only a pak (Files, Pak Store...) can change Tools/.
rebuild_cache() {
	: > "$CACHE"
	for pak in "$SDCARD_PATH"/Tools/"$PLATFORM"/*.pak; do
		[ -d "$pak" ] || continue
		for event in boot pre-launch post-launch pre-sleep post-resume; do
			if [ -x "$pak/$event.sh" ]; then
				echo "$pak" >> "$CACHE"
				break
			fi
		done
	done
}

run_event() {
	[ -f "$CACHE" ] || rebuild_cache

	VETOED=0
	while IFS= read -r pak; do
		[ -x "$pak/$1.sh" ] || continue
		# own subshell, output discarded: a broken hook cannot take the
		# launcher down, and must log to its own file if it wants to be heard
		( "$pak/$1.sh" ) > /dev/null 2>&1 || VETOED=1
	done < "$CACHE"

	if [ "$1" = "post-launch" ] && [ "$HOOK_TYPE" = "pak" ]; then
		rebuild_cache
	fi

	# only run_hooks.sh pre-launch.d acts on this, every other event ignores it
	return $VETOED
}

case "$1" in
	rebuild-cache) rebuild_cache ;;
	boot|pre-launch|post-launch|pre-sleep|post-resume) run_event "$1" ;;
	*) exit 0 ;;
esac

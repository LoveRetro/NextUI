#!/bin/sh
# run_hooks.sh - shared hook runner for NextUI
# Usage: run_hooks.sh <dir-name> [--sync-only]
#
# dir-name:   directory name under $USERDATA_PATH/.hooks/ (e.g. pre-launch.d, boot.d)
# --sync-only: force all scripts to run synchronously
#
# By default, scripts run in the background. Scripts ending in .sync.sh
# always run synchronously. All background scripts are waited on before exit.
#
# Veto: a synchronous hook that exits non-zero makes run_hooks.sh itself exit
# non-zero, letting the caller cancel whatever the hooks were run ahead of
# (see pre-launch.d in MinUI.pak/launch.sh). Background hooks cannot veto --
# their exit status is not recoverable from `wait` in POSIX sh -- so only
# .sync.sh hooks (or any hook under --sync-only) get a vote.

DIR_NAME="$1"
SYNC_ONLY="${2:-}"

: "${SDCARD_PATH:=/var/tmp/nextui/sdcard}"
: "${PLATFORM:=desktop}"
: "${USERDATA_PATH:=$SDCARD_PATH/.userdata/$PLATFORM}"

HOOK_DIR="$USERDATA_PATH/.hooks/$DIR_NAME"
[ -d "$HOOK_DIR" ] || exit 0

case "$DIR_NAME" in
	pre-*)  export HOOK_PHASE="pre" ;;
	post-*) export HOOK_PHASE="post" ;;
	boot*)  export HOOK_PHASE="boot" ;;
esac
export HOOK_CATEGORY="$DIR_NAME"

VETOED=0

for script in "$HOOK_DIR"/*.sh; do
	[ -f "$script" ] || continue
	if [ "$SYNC_ONLY" = "--sync-only" ] || echo "$script" | grep -q '\.sync\.sh$'; then
		if ! ( "$script" ) > /dev/null 2>&1; then
			VETOED=1
		fi
	else
		( "$script" ) > /dev/null 2>&1 &
	fi
done
wait

exit $VETOED

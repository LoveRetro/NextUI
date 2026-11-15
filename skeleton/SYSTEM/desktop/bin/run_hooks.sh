#!/bin/sh
# run_hooks.sh - shared hook runner for NextUI
# Usage: run_hooks.sh <dir-name> [--sync-only]
#
# dir-name:   directory name under $USERDATA_PATH/.hooks/ (e.g. pre-launch.d, boot.d)
# --sync-only: force all scripts to run synchronously
#
# By default, scripts run in the background. Scripts ending in .sync.sh
# always run synchronously. All background scripts are waited on before exit.

DIR_NAME="$1"
SYNC_ONLY="${2:-}"

: "${SDCARD_PATH:=/var/tmp/nextui/sdcard}"
: "${PLATFORM:=desktop}"
: "${USERDATA_PATH:=$SDCARD_PATH/.userdata/$PLATFORM}"

# Pak-scoped hooks run first, and are the only ones that can veto
# (a pre-launch.d caller cancels the launch on a non-zero exit). See HOOKS.md.
"$(dirname "$0")/pak-hooks.sh" "${DIR_NAME%.d}"
VETOED=$?

HOOK_DIR="$USERDATA_PATH/.hooks/$DIR_NAME"
[ -d "$HOOK_DIR" ] || exit $VETOED

case "$DIR_NAME" in
	pre-*)  export HOOK_PHASE="pre" ;;
	post-*) export HOOK_PHASE="post" ;;
	boot*)  export HOOK_PHASE="boot" ;;
esac
export HOOK_CATEGORY="$DIR_NAME"

for script in "$HOOK_DIR"/*.sh; do
	[ -f "$script" ] || continue
	if [ "$SYNC_ONLY" = "--sync-only" ] || echo "$script" | grep -q '\.sync\.sh$'; then
		( "$script" ) > /dev/null 2>&1 || true
	else
		( "$script" ) > /dev/null 2>&1 &
	fi
done
wait
exit $VETOED

#!/bin/sh
# run_hooks.sh - DEPRECATED legacy hook runner for $USERDATA_PATH/.hooks/
#
# Superseded by pak-hooks.sh: self-registering, no arming step, no orphaned
# scripts left behind when a pak is deleted. See HOOKS.md. Kept only so a
# pak that already dropped scripts into .hooks/ doesn't silently stop
# working -- do not build anything new against this mechanism.
#
# Usage: run_hooks.sh <dir-name> [--sync-only]
#
# dir-name:   directory name under $USERDATA_PATH/.hooks/ (e.g. pre-launch.d, boot.d)
# --sync-only: force all scripts to run synchronously
#
# By default, scripts run in the background. Scripts ending in .sync.sh
# always run synchronously. All background scripts are waited on before exit.

DIR_NAME="$1"
SYNC_ONLY="${2:-}"

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${USERDATA_PATH:=$SDCARD_PATH/.userdata/$PLATFORM}"
: "${LOGS_PATH:=$USERDATA_PATH/logs}"

HOOK_DIR="$USERDATA_PATH/.hooks/$DIR_NAME"
[ -d "$HOOK_DIR" ] || exit 0

# only warn when there's actually a script to run here -- an unused,
# never-populated .hooks/ directory shouldn't spam the log
for _probe in "$HOOK_DIR"/*.sh; do
	[ -f "$_probe" ] || continue
	echo "$(date): DEPRECATED - $HOOK_DIR/$(basename "$_probe") uses the legacy .hooks/ mechanism (run_hooks.sh). Migrate to a pak-scoped hook (pak-hooks.sh) -- see HOOKS.md." >> "$LOGS_PATH/hooks-deprecated.txt"
	break
done

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

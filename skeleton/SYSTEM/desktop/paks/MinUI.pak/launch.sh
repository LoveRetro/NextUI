#!/bin/sh
# MinUI.pak

export PLATFORM="desktop"
# I put some thinking into what path to use, this is the only one that ticks all the boxes:
# - writable by normal user
# - not likely to be accidentally deleted by user
# - not in home directory to avoid having to expand ~ or $HOME in #defines or scripts
# - clearly for temporary/debug use only
# - works on both macOS and Linux
export SDCARD_PATH="/var/tmp/nextui/sdcard"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export CORES_PATH="$SYSTEM_PATH/cores"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export IS_NEXT="yes"

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=$LD_LIBRARY_PATH:$DYLD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:$PATH

#batmon.elf & # &> $SDCARD_PATH/batmon.txt &

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

cd $(dirname "$0")

#######################################
# Hook system

HOOKS_DIR="$USERDATA_PATH/.hooks"

run_hooks() {
	_hook_phase="$1"
	_hook_dir="$HOOKS_DIR/${_hook_phase}-launch.d"
	[ -d "$_hook_dir" ] || return 0
	for _hook_script in "$_hook_dir"/*.sh; do
		[ -f "$_hook_script" ] || continue
		( export HOOK_PHASE="$_hook_phase"; "$_hook_script" ) > /dev/null 2>&1 || true
	done
}

parse_hook_cmd() {
	HOOK_CMD="$1"
	HOOK_EMU_PATH=$(echo "$HOOK_CMD" | sed "s/^'\\([^']*\\)'.*/\\1/")
	_remainder=$(echo "$HOOK_CMD" | sed "s/^'[^']*'//")
	if echo "$_remainder" | grep -q "'"; then
		HOOK_TYPE="rom"
		HOOK_ROM_PATH=$(echo "$_remainder" | sed "s/.*'\\([^']*\\)'.*/\\1/")
	else
		HOOK_TYPE="pak"
		HOOK_ROM_PATH=""
	fi
	[ -f /tmp/last.txt ] && HOOK_LAST=$(cat /tmp/last.txt) || HOOK_LAST=""
	export HOOK_CMD HOOK_EMU_PATH HOOK_TYPE HOOK_ROM_PATH HOOK_LAST
}

#######################################

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
#while [ -f $EXEC_PATH ]; do
	nextui.elf # &> $LOGS_PATH/nextui.txt
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		parse_hook_cmd "$CMD"
		run_hooks pre
		eval $CMD
		run_hooks post
		rm -f $NEXT_PATH
	fi
#done

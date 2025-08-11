#!/bin/sh

export PLATFORM="tg5040"
export SDCARD_PATH="/mnt/SDCARD"
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

export TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	export DEVICE="brick"
fi

export IS_NEXT="yes"

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:/usr/trimui/bin:$PATH

export HOME=$USERDATA_PATH

cd $SYSTEM_PATH/dbg
killall gdbserver
./gdbserver "$@"
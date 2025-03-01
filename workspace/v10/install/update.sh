#!/bin/sh

# becomes /.system/v10/bin/install.sh

# clean up from an previous ill-considered update
DTB_PATH=/storage/.system/v10/dat/rk3562-magicx-linux.dtb
if [ -f "$DTB_PATH" ]; then
	rm -rf "$DTB_PATH"
fi


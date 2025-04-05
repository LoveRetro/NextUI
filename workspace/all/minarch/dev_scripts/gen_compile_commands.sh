#!/bin/bash

which bear>/dev/null
if [ $? -ne 0 ]; then
    echo "Please install bear:"
    echo "brew install bear"
    exit 1
fi

SCRIPT_DIRNAME=$(dirname "$0")
pushd "$SCRIPT_DIRNAME" > /dev/null
SCRIPT_PATH=`pwd`
popd > /dev/null

cd "$SCRIPT_PATH/.."

bear -- "$SCRIPT_PATH/compile_mac_os.sh"

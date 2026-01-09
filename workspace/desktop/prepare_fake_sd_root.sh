#!/bin/bash

# Prepares a faux SD card structure for debugging on desktop
# macOS: /var/tmp/nextui/sdcard
# Linux: /var/tmp/nextui/sdcard

# 1. Check if it already exists, we will call this from Makefile. If already prepared, bail and do nothing
# 2. Copy folder structure from skeleton/(BASE,EXTRAS,SYSTEM) into the folder
set -euo pipefail

TARGET="/var/tmp/nextui/sdcard"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKELETON_DIR="$(cd "$SCRIPT_DIR/../../skeleton" && pwd)"

# Bail if already prepared
if [ -d "$TARGET" ]; then
    echo "SD root already exists at: $TARGET"
    exit 0
fi

# Ensure skeleton exists
if [ ! -d "$SKELETON_DIR" ]; then
    echo "Skeleton directory not found: $SKELETON_DIR" >&2
    exit 1
fi

# Create target
mkdir -p "$TARGET"

# Copy structure in specific order: BASE, then EXTRAS, then SYSTEM (into .system)
for SUBDIR in BASE EXTRAS SYSTEM; do
    SOURCE_PATH="$SKELETON_DIR/$SUBDIR"
    if [ -d "$SOURCE_PATH" ]; then
        if [ "$SUBDIR" = "SYSTEM" ]; then
            DEST="$TARGET/.system"
        else
            DEST="$TARGET"
        fi
        mkdir -p "$DEST"
        if command -v rsync >/dev/null 2>&1; then
            rsync -a "$SOURCE_PATH"/ "$DEST"/
        else
            cp -R "$SOURCE_PATH"/. "$DEST"/
        fi
    fi
done

echo "Prepared faux SD root at: $TARGET"

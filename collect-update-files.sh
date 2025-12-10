#!/bin/bash
# Script to collect all updated files for tg5040 into an update folder

set -e

PLATFORM="tg5040"
UPDATE_DIR="tg5040-update"
BIN_DIR="${UPDATE_DIR}/.system/${PLATFORM}/bin"

echo "Collecting update files for ${PLATFORM}..."

# Create directory structure
mkdir -p "${BIN_DIR}"

# Check if files exist and copy them
FILES_COPIED=0

# Copy nextui.elf
if [ -f "workspace/all/nextui/build/${PLATFORM}/nextui.elf" ]; then
    echo "✓ Copying nextui.elf..."
    cp "workspace/all/nextui/build/${PLATFORM}/nextui.elf" "${BIN_DIR}/"
    FILES_COPIED=$((FILES_COPIED + 1))
else
    echo "✗ WARNING: nextui.elf not found! Run 'make PLATFORM=${PLATFORM} build' first."
fi

# Copy minarch.elf
if [ -f "workspace/all/minarch/build/${PLATFORM}/minarch.elf" ]; then
    echo "✓ Copying minarch.elf..."
    cp "workspace/all/minarch/build/${PLATFORM}/minarch.elf" "${BIN_DIR}/"
    FILES_COPIED=$((FILES_COPIED + 1))
else
    echo "✗ WARNING: minarch.elf not found! Run 'make PLATFORM=${PLATFORM} build' first."
fi

# Copy suspend script
if [ -f "skeleton/SYSTEM/${PLATFORM}/bin/suspend" ]; then
    echo "✓ Copying suspend script..."
    cp "skeleton/SYSTEM/${PLATFORM}/bin/suspend" "${BIN_DIR}/"
    chmod +x "${BIN_DIR}/suspend"
    FILES_COPIED=$((FILES_COPIED + 1))
else
    echo "✗ WARNING: suspend script not found!"
fi

# Create a README with file info
cat > "${UPDATE_DIR}/README.txt" << EOF
TG5040 Performance Optimizations Update
=======================================

This update contains:
- nextui.elf: Main UI with CPU scaling, memory, and array optimizations
- minarch.elf: Emulator launcher with I/O and memory optimizations  
- suspend: Deep sleep charging fix

Files to update on device:
  /mnt/SDCARD/.system/${PLATFORM}/bin/nextui.elf
  /mnt/SDCARD/.system/${PLATFORM}/bin/minarch.elf
  /mnt/SDCARD/.system/${PLATFORM}/bin/suspend

Optimizations included:
- CPU frequency scaling with hysteresis (reduced power consumption)
- Memory access pattern optimizations (better cache usage)
- Loop unrolling and compiler optimizations for Cortex-A53
- Deep sleep charging support

Build date: $(date)
Branch: $(git branch --show-current 2>/dev/null || echo "unknown")
Commit: $(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
EOF

echo ""
if [ $FILES_COPIED -eq 3 ]; then
    echo "✓ Successfully collected ${FILES_COPIED} files to ${UPDATE_DIR}/"
    echo ""
    echo "Directory structure:"
    tree -L 3 "${UPDATE_DIR}" 2>/dev/null || find "${UPDATE_DIR}" -type f
    echo ""
    echo "Next steps:"
    echo "1. Copy the '${UPDATE_DIR}' folder to your device's SD card"
    echo "2. Run the update script on your device:"
    echo "   /mnt/SDCARD/${UPDATE_DIR}/update-on-device.sh"
else
    echo "⚠ Only ${FILES_COPIED}/3 files collected. Please build first!"
    exit 1
fi




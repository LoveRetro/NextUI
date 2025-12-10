#!/bin/sh
# Script to backup and update files on the TG5040 device
# Run this script on the device itself (not on the build machine)

set -e

PLATFORM="tg5040"
SDCARD_PATH="/mnt/SDCARD"
SYSTEM_BIN="${SDCARD_PATH}/.system/${PLATFORM}/bin"
UPDATE_DIR="${SDCARD_PATH}/tg5040-update"
BACKUP_DIR="${SDCARD_PATH}/.system/${PLATFORM}/bin-backup-$(date +%Y%m%d_%H%M%S)"

echo "=========================================="
echo "TG5040 Performance Optimizations Update"
echo "=========================================="
echo ""

# Check if update directory exists
if [ ! -d "${UPDATE_DIR}" ]; then
    echo "ERROR: Update directory not found: ${UPDATE_DIR}"
    echo "Please copy the 'tg5040-update' folder to your SD card first."
    exit 1
fi

# Check if system directory exists
if [ ! -d "${SYSTEM_BIN}" ]; then
    echo "ERROR: System bin directory not found: ${SYSTEM_BIN}"
    exit 1
fi

# Create backup directory
echo "Creating backup..."
mkdir -p "${BACKUP_DIR}"

# Backup existing files
FILES_TO_UPDATE="nextui.elf minarch.elf suspend"
BACKED_UP=0

for file in ${FILES_TO_UPDATE}; do
    if [ -f "${SYSTEM_BIN}/${file}" ]; then
        echo "  Backing up ${file}..."
        cp "${SYSTEM_BIN}/${file}" "${BACKUP_DIR}/"
        BACKED_UP=$((BACKED_UP + 1))
    else
        echo "  WARNING: ${file} not found (will be installed as new)"
    fi
done

echo "  Backed up ${BACKED_UP} file(s) to ${BACKUP_DIR}"
echo ""

# Update files
echo "Installing updates..."
UPDATED=0

for file in ${FILES_TO_UPDATE}; do
    if [ -f "${UPDATE_DIR}/.system/${PLATFORM}/bin/${file}" ]; then
        echo "  Installing ${file}..."
        cp "${UPDATE_DIR}/.system/${PLATFORM}/bin/${file}" "${SYSTEM_BIN}/"
        chmod +x "${SYSTEM_BIN}/${file}" 2>/dev/null || true
        UPDATED=$((UPDATED + 1))
    else
        echo "  WARNING: ${file} not found in update package"
    fi
done

echo ""
if [ $UPDATED -gt 0 ]; then
    echo "✓ Successfully updated ${UPDATED} file(s)"
    echo ""
    echo "Backup location: ${BACKUP_DIR}"
    echo ""
    echo "To restore backup, run:"
    echo "  cp ${BACKUP_DIR}/* ${SYSTEM_BIN}/"
    echo ""
    echo "Update complete! Please reboot your device."
else
    echo "✗ No files were updated. Check the update directory."
    exit 1
fi




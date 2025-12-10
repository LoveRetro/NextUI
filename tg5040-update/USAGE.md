# TG5040 Performance Optimizations Update Guide

## Quick Start

### Step 1: Build the optimized binaries
```bash
make PLATFORM=tg5040 clean
make PLATFORM=tg5040 build COMPILE_CORES=
```

### Step 2: Collect update files
```bash
./collect-update-files.sh
```

This will create a `tg5040-update/` folder with all the files needed.

### Step 3: Copy to SD card
Copy the entire `tg5040-update/` folder to the root of your device's SD card.

### Step 4: Run update on device
On your TG5040 device, run:
```bash
/mnt/SDCARD/tg5040-update/update-on-device.sh
```

Or via ADB:
```bash
adb shell /mnt/SDCARD/tg5040-update/update-on-device.sh
```

### Step 5: Reboot
Reboot your device to apply the changes.

## What Gets Updated

- **nextui.elf** - Main UI with performance optimizations
- **minarch.elf** - Emulator launcher with I/O optimizations
- **suspend** - Deep sleep charging fix

## Backup & Restore

The update script automatically creates a backup in:
```
/mnt/SDCARD/.system/tg5040/bin-backup-YYYYMMDD_HHMMSS/
```

To restore the backup:
```bash
cp /mnt/SDCARD/.system/tg5040/bin-backup-*/.* /mnt/SDCARD/.system/tg5040/bin/
```

## Manual Update (Alternative)

If you prefer to update manually:

1. Backup existing files:
```bash
cp /mnt/SDCARD/.system/tg5040/bin/nextui.elf /mnt/SDCARD/.system/tg5040/bin/nextui.elf.backup
cp /mnt/SDCARD/.system/tg5040/bin/minarch.elf /mnt/SDCARD/.system/tg5040/bin/minarch.elf.backup
cp /mnt/SDCARD/.system/tg5040/bin/suspend /mnt/SDCARD/.system/tg5040/bin/suspend.backup
```

2. Copy new files:
```bash
cp /mnt/SDCARD/tg5040-update/.system/tg5040/bin/* /mnt/SDCARD/.system/tg5040/bin/
chmod +x /mnt/SDCARD/.system/tg5040/bin/suspend
```

3. Reboot




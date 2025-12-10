TG5040 Performance Optimizations Update
=======================================

This update contains:
- nextui.elf: Main UI with CPU scaling, memory, and array optimizations
- minarch.elf: Emulator launcher with I/O and memory optimizations  
- suspend: Deep sleep charging fix

Files to update on device:
  /mnt/SDCARD/.system/tg5040/bin/nextui.elf
  /mnt/SDCARD/.system/tg5040/bin/minarch.elf
  /mnt/SDCARD/.system/tg5040/bin/suspend

Optimizations included:
- CPU frequency scaling with hysteresis (reduced power consumption)
- Memory access pattern optimizations (better cache usage)
- Loop unrolling and compiler optimizations for Cortex-A53
- Deep sleep charging support

Build date: Wed Dec 10 03:18:01 PM CST 2025
Branch: tg5040-performance-optimizations
Commit: 76ee009

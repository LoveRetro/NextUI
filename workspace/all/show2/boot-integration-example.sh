#!/bin/sh
# boot.sh modification example - Using show2.elf instead of show.elf
# This shows how to replace show.elf calls with show2.elf

# BEFORE: Using show.elf (old way)
# ./show.elf ./$DEVICE/installing.png

# AFTER: Using show2.elf 

# Option 1: Simple replacement (runs until killed)
# ./show2.elf --mode=simple --image=./$DEVICE/installing.png &
# SHOW_PID=$!
# # ... do work ...
# kill $SHOW_PID

# Option 2: With progress bar (runs until killed)
# ./show2.elf --mode=progress --image=./$DEVICE/logo.png --text="Installing..." --progress=0 &
# SHOW_PID=$!
# # ... do work ...
# kill $SHOW_PID

# Option 3: Interactive with daemon mode (recommended for complex operations)
# Start daemon
# ./show2.elf --mode=daemon --image=./$DEVICE/logo.png --bgcolor=0x000000 --text="Preparing installation..." &
# sleep 0.5  # Give daemon time to start

# Then during your installation process:
# echo "TEXT:Extracting package..." > /tmp/show2.fifo
# echo "PROGRESS:10" > /tmp/show2.fifo
# ... extract files ...

# echo "TEXT:Installing system files..." > /tmp/show2.fifo  
# echo "PROGRESS:40" > /tmp/show2.fifo
# ... copy files ...

# echo "TEXT:Finalizing..." > /tmp/show2.fifo
# echo "PROGRESS:90" > /tmp/show2.fifo
# ... finish up ...

# echo "PROGRESS:100" > /tmp/show2.fifo
# echo "QUIT" > /tmp/show2.fifo

# ============================================================================
# EXAMPLE: Modified boot.sh section for package installation
# ============================================================================

install_with_progress() {
    # Start show2 in daemon mode
    ./show2.elf --mode=daemon --image=./$DEVICE/logo.png --bgcolor=0x000000 --text="Starting installation..." &
    sleep 0.5
    
    local progress=0
    local step_size=0
    
    # Count pakz files
    local pakz_count=0
    for pakz in $PAKZ_PATH; do
        if [ -e "$pakz" ]; then
            pakz_count=$((pakz_count + 1))
        fi
    done
    
    if [ $pakz_count -gt 0 ]; then
        step_size=$((80 / pakz_count))  # Reserve 80% for packages, 20% for finalization
    fi
    
    # Install packages with progress
    for pakz in $PAKZ_PATH; do
        if [ ! -e "$pakz" ]; then continue; fi
        
        local pakz_name=$(basename "$pakz")
        echo "TEXT:Installing $pakz_name..." > /tmp/show2.fifo
        echo "PROGRESS:$progress" > /tmp/show2.fifo
        
        ./unzip -o -d "$SDCARD_PATH" "$pakz"
        rm -f "$pakz"
        
        progress=$((progress + step_size))
        echo "PROGRESS:$progress" > /tmp/show2.fifo
        
        # Run postinstall if present
        if [ -f $SDCARD_PATH/post_install.sh ]; then
            echo "TEXT:Running post-install for $pakz_name..." > /tmp/show2.fifo
            $SDCARD_PATH/post_install.sh
            rm -f $SDCARD_PATH/post_install.sh
        fi
    done
    
    # Finalization
    echo "TEXT:Finalizing installation..." > /tmp/show2.fifo
    echo "PROGRESS:90" > /tmp/show2.fifo
    sync
    
    echo "PROGRESS:100" > /tmp/show2.fifo
    echo "TEXT:Installation complete!" > /tmp/show2.fifo
    sleep 1
    
    echo "QUIT" > /tmp/show2.fifo
}

# ============================================================================
# EXAMPLE: Modified update installation section
# ============================================================================

install_update_with_progress() {
    if [ -f "$UPDATE_PATH" ]; then
        cd $(dirname "$0")/$PLATFORM
        
        # Determine message
        local message="Installing NextUI..."
        if [ -d "$SYSTEM_PATH" ]; then
            message="Updating NextUI..."
        fi
        
        # Start daemon
        ./show2.elf --mode=daemon --image=./$DEVICE/logo.png --bgcolor=0x000000 --text="$message" &
        sleep 0.5
        
        # Clean replacement
        echo "TEXT:Cleaning old files..." > /tmp/show2.fifo
        echo "PROGRESS:10" > /tmp/show2.fifo
        rm -rf $SYSTEM_PATH/$PLATFORM/bin
        rm -rf $SYSTEM_PATH/$PLATFORM/lib
        rm -rf $SYSTEM_PATH/$PLATFORM/paks/MinUI.pak
        
        # Extract
        echo "TEXT:Extracting update..." > /tmp/show2.fifo
        echo "PROGRESS:30" > /tmp/show2.fifo
        ./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH"
        
        echo "PROGRESS:70" > /tmp/show2.fifo
        rm -f "$UPDATE_PATH"
        
        # Run install script
        if [ -f $SYSTEM_PATH/$PLATFORM/bin/install.sh ]; then
            echo "TEXT:Running installation script..." > /tmp/show2.fifo
            echo "PROGRESS:80" > /tmp/show2.fifo
            $SYSTEM_PATH/$PLATFORM/bin/install.sh
        fi
        
        # Complete
        echo "PROGRESS:100" > /tmp/show2.fifo
        echo "TEXT:Update complete!" > /tmp/show2.fifo
        sleep 1
        echo "QUIT" > /tmp/show2.fifo
    fi
}

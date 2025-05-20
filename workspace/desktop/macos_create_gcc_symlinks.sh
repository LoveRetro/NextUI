#!/bin/bash

# Check if script is run with sudo
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Preserve the original user's environment for brew command
if [ ! -z "$SUDO_USER" ]; then
    BREW_PATH=$(sudo -u "$SUDO_USER" which brew)
    if [ -z "$BREW_PATH" ]; then
        echo "Error: Homebrew not found in $SUDO_USER's environment"
        exit 1
    fi
else
    echo "Error: SUDO_USER not set. Please run with sudo."
    exit 1
fi

# Directory settings
TARGET_DIR="/usr/local/bin"

# Find GCC installation using the original user's brew
if ! GCC_BASE=$(sudo -u "$SUDO_USER" "$BREW_PATH" --prefix gcc 2>/dev/null); then
    echo "Error: GCC is not installed via Homebrew"
    exit 1
fi

GCC_BIN_DIR="$GCC_BASE/bin"

if [ ! -d "$GCC_BIN_DIR" ]; then
    echo "Error: GCC binary directory not found at $GCC_BIN_DIR"
    exit 1
fi

echo "Using GCC binaries from: $GCC_BIN_DIR"

# Ensure target directory exists
if [ ! -d "$TARGET_DIR" ]; then
    echo "Creating directory $TARGET_DIR..."
    mkdir -p "$TARGET_DIR"
    chmod 755 "$TARGET_DIR"
fi

# Function to create a symlink for a GCC tool
create_symlink() {
    local source_file="$1"
    local target_name="$2"  # Now accepts an optional specific target name
    local source_name=$(basename "$source_file")
    
    # Skip architecture-specific tools
    if [[ "$source_name" == aarch64-apple-darwin* ]]; then
        return 0
    fi
    
    # If no specific target name provided, generate it from source name
    if [ -z "$target_name" ]; then
        if [[ $source_name =~ ^(.*)-[0-9]+$ ]]; then
            target_name="${BASH_REMATCH[1]}"
        else
            target_name="$source_name"
        fi
    fi
    
    local target_path="$TARGET_DIR/$target_name"

    # Check if source exists and is executable
    if [ ! -x "$source_file" ]; then
        echo "Error: Source file $source_file is not executable or doesn't exist"
        return 1
    fi

    # Handle existing symlink
    if [ -L "$target_path" ]; then
        local current_target=$(readlink "$target_path")
        if [ "$current_target" = "$source_file" ]; then
            echo "Skipping $target_name: Link already exists and points to correct target"
            return 0
        else
            echo "Removing existing symlink $target_path..."
            rm "$target_path"
        fi
    elif [ -e "$target_path" ]; then
        echo "Error: $target_path exists and is not a symlink"
        return 1
    fi

    # Create new symlink
    echo "Creating symlink: $target_name -> $source_file"
    ln -s "$source_file" "$target_path"

    # Verify symlink
    if [ ! -L "$target_path" ] || [ ! -x "$target_path" ]; then
        echo "Error: Failed to create working symlink for $target_name"
        return 1
    fi
}

# Clean up existing directory
rm -f "$TARGET_DIR"/*

# Process GCC tools
echo "Processing GCC tools from $GCC_BIN_DIR..."

# Find and process all versioned GCC tools
for tool in "$GCC_BIN_DIR"/*-[0-9]*; do
    # Skip non-existent files (in case no matches found)
    [ -e "$tool" ] || continue
    
    # Skip if not a regular file or not executable
    [ -f "$tool" ] && [ -x "$tool" ] || continue
    
    create_symlink "$tool"
    
    # Special case for gcc-ar: create additional 'ar' symlink
    if [[ $(basename "$tool") == "gcc-ar-14" ]]; then
        create_symlink "$tool" "ar"
    fi
done

# Special case for gfortran (which might not have version suffix)
if [ -x "$GCC_BIN_DIR/gfortran" ]; then
    create_symlink "$GCC_BIN_DIR/gfortran"
fi

echo "Finished creating symlinks"

# Final verification
echo "Verifying created symlinks..."
ls -l "$TARGET_DIR"


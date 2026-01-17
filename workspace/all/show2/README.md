# show2.elf - Enhanced Loading Screen Tool

An improved replacement for `show.elf` and `sdl2display` with support for multiple display modes, progress bars, and runtime updates.

## Features

- **Three Display Modes**:
  1. **Simple Mode**: Display a centered image/logo with optional delay
  2. **Progress Mode**: Show logo + progress bar + text (single render)
  3. **Daemon Mode**: Interactive mode with runtime updates via FIFO

- **Flexible Configuration**:
  - Custom background colors (hex format)
  - Dynamic text updates
  - Progress bar (0-100%)
  - Runtime updates without restarting

- **Single Binary**: One executable handles all use cases

## Usage

### Simple Mode
Display a centered image with an optional background color (runs until killed):

```bash
./show2.elf --mode=simple --image=<path> [--bgcolor=0x000000]
```

**Examples:**
```bash
# Show splash.png until killed
./show2.elf --mode=simple --image=splash.png

# Show with black background
./show2.elf --mode=simple --image=splash.png --bgcolor=0x000000

# Show with custom background color
./show2.elf --mode=simple --image=logo.png --bgcolor=0x1a2b3c
```

### Progress Mode
Display logo with progress bar and text (runs until killed):

```bash
./show2.elf --mode=progress --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF] [--text="message"] [--progress=0]
```

**Examples:**
```bash
# Show logo with "Installing..." text at 0% progress
./show2.elf --mode=progress --image=logo.png --text="Installing..." --progress=0

# Show with 50% progress and custom colors
./show2.elf --mode=progress --image=logo.png --bgcolor=0x000000 --fontcolor=0xFFFFFF --text="Installing system files..." --progress=50

# Custom background and red text
./show2.elf --mode=progress --image=logo.png --bgcolor=0x1e1e1e --fontcolor=0xFF0000 --text="Please wait..." --progress=75
```

### Daemon Mode
Start an interactive session that accepts runtime updates via FIFO:

```bash
./show2.elf --mode=daemon --image=<path> [--bgcolor=0x000000] [--fontcolor=0xFFFFFF] [--text="message"]
```

**Examples:**
```bash
# Start daemon with initial text
./show2.elf --mode=daemon --image=logo.png --bgcolor=0x000000 --text="Initializing..." &

# Update text during runtime
echo "TEXT:Installing system..." > /tmp/show2.fifo

# Update progress
echo "PROGRESS:25" > /tmp/show2.fifo

# Update background color
echo "BGCOLOR:0x003366" > /tmp/show2.fifo

# Update font color
echo "FONTCOLOR:0xFF0000" > /tmp/show2.fifo

# Quit the daemon
echo "QUIT" > /tmp/show2.fifo
```

## Daemon Mode Commands

When running in daemon mode, send commands to `/tmp/show2.fifo`:

| Command | Description | Example |
|---------|-------------|---------|
| `TEXT:<message>` | Update the text message | `echo "TEXT:Loading files..." > /tmp/show2.fifo` |
| `PROGRESS:<0-100>` | Update progress bar | `echo "PROGRESS:50" > /tmp/show2.fifo` |
| `BGCOLOR:<hex>` | Change background color | `echo "BGCOLOR:0xFF0000" > /tmp/show2.fifo` || `FONTCOLOR:<hex>` | Change font color | `echo "FONTCOLOR:0x00FF00" > /tmp/show2.fifo` || `QUIT` | Exit the daemon | `echo "QUIT" > /tmp/show2.fifo` |

## Integration Examples

### Replace show.elf in boot.sh

**Before:**
```bash
./show.elf ./installing.png
```

**After (simple mode):**
```bash
./show2.elf --mode=simple --image=./installing.png &
SHOW_PID=$!
# ... do installation work ...
kill $SHOW_PID
```

**After (daemon mode with progress):**
```bash
./show2.elf --mode=daemon --image=./logo.png --text="Installing..." &
# ... do installation work ...
echo "PROGRESS:50" > /tmp/show2.fifo
# ... more work ...
echo "QUIT" > /tmp/show2.fifo
```

### Multi-step Installation Process

```bash
#!/bin/bash

# Start daemon
./show2.elf --mode=daemon --image=./logo.png --bgcolor=0x000000 --text="Starting installation..." &

# Step 1: Extract files
echo "TEXT:Extracting files..." > /tmp/show2.fifo
echo "PROGRESS:10" > /tmp/show2.fifo
unzip package.zip
echo "PROGRESS:30" > /tmp/show2.fifo

# Step 2: Copy system files
echo "TEXT:Installing system files..." > /tmp/show2.fifo
echo "PROGRESS:40" > /tmp/show2.fifo
cp -r files/* /destination/
echo "PROGRESS:70" > /tmp/show2.fifo

# Step 3: Finalize
echo "TEXT:Finalizing installation..." > /tmp/show2.fifo
echo "PROGRESS:90" > /tmp/show2.fifo
sync
echo "PROGRESS:100" > /tmp/show2.fifo

# Complete
sleep 1
echo "TEXT:Installation complete!" > /tmp/show2.fifo
sleep 2
echo "QUIT" > /tmp/show2.fifo
```

## Color Format

Background colors can be specified in multiple formats:
- `0xRRGGBB` (e.g., `0xFF0000` for red)
- `RRGGBB` (e.g., `00FF00` for green)
- `#RRGGBB` (e.g., `#0000FF` for blue)

Common colors:
- Black: `0x000000`
- White: `0xFFFFFF`
- Dark Gray: `0x1a1a1a`
- Navy Blue: `0x003366`

## Building

Compile within the Docker build environment:

```bash
make build PLATFORM=tg5050
```

Or manually in the toolchain:
```bash
cd workspace/tg5050/show2
make
```

## Notes

- Font is embedded in the binary - no external font files needed
- Simple and progress modes run until killed (Ctrl+C or external signal)
- Daemon mode can be exited via FIFO QUIT command or Ctrl+C
- In daemon mode, the FIFO blocks until a reader connects
- The tool automatically cleans up the FIFO on exit
- All colors support hex format: 0xRRGGBB, RRGGBB, or #RRGGBB

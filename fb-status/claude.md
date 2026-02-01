# fb_status - Framebuffer Status Display

> **Path mapping**: This directory (`/shared/dev/anycubic/fb-status`) is mapped to `Z:\anycubic\fb-status` on the Windows development host.

Generic lightweight tool for displaying status messages on Anycubic printer LCD screens. Not tied to any specific application. Used by ACProxyCam BedMesh calibration scripts to show progress during long-running operations.

## Overview

Renders text directly to the Linux framebuffer (/dev/fb0) using stb_truetype for dynamic font rendering. Screen save/restore uses ffmpeg for reliability. Auto-detects printer model and applies correct screen orientation.

## Architecture

```
fb_status save              →  ffmpeg captures /dev/fb0 to BMP
fb_status show "message"    →  stb_truetype renders text to framebuffer
fb_status hide              →  ffmpeg restores BMP to /dev/fb0
fb_status busy              →  Native API: set printer busy
fb_status free              →  Native API: set printer free
fb_status pipe              →  Long-running mode with stdin commands
```

## Build

```bash
cd /shared/dev/anycubic/fb-status   # or Z:\anycubic\fb-status on Windows
make
make deploy PRINTER_IP=192.168.178.43
```

Requires: RV1106 ARM toolchain at /shared/dev/rv1106-toolchain

## Usage

### One-shot Commands

```bash
# Basic usage
fb_status save                           # Save current screen
fb_status show "Status message"          # Show green text
fb_status hide                           # Restore original screen

# With options
fb_status show "Error!" -c red           # Red text
fb_status show "Warning" -c yellow -g black  # Yellow on black
fb_status show "Custom" -c FF00FF        # Magenta (hex RGB)
fb_status show "Large" -s 48             # 48px font size
fb_status show "Top msg" -p top          # Position at top
fb_status show "Quick" -t 3              # Auto-hide after 3 seconds
fb_status show "Bold" -B                 # Bold font
fb_status show "Busy msg" -b             # Also set printer busy

# Multiline support (use \n)
fb_status show "Line 1\nLine 2\nLine 3"  # Multiple lines

# Printer busy/free status
fb_status busy                           # Set printer busy via Native API
fb_status free                           # Set printer free

# Combined operations
fb_status show "Starting..." -b          # Show message AND set busy
fb_status hide -f                        # Hide AND set free
```

### Pipe Mode (Long-running Scripts)

For scripts that run for extended periods and need to update the display dynamically:

```bash
# Start pipe mode
fb_status pipe [options]

# Options for pipe mode:
#   -m "message"   Initial message to display
#   -q             Quiet mode (no OK/ERR responses)
#   -b             Set printer busy on start
#   -c, -g, -s, -p, -B, -I, -F  Set initial display options
```

#### Pipe Mode Commands (via stdin)

| Command | Description |
|---------|-------------|
| `busy` | Set printer busy via Native API |
| `free` | Set printer free via Native API |
| `show <message>` | Display message (supports `\n` for multiline) |
| `color <color>` | Set text color for subsequent shows |
| `bg <color>` | Set background color |
| `size <n>` | Set font size (8-200) |
| `position <pos>` | Set position: top, center, bottom |
| `bold` | Enable bold font |
| `italic` | Enable italic font |
| `regular` | Reset to regular font |
| `font <path>` | Load custom font file |
| `hide` | Temporarily restore saved screen |
| `close` | Restore screen and exit |
| `close <secs>` | Wait N seconds, then close |
| `close <secs> <msg>` | Show message, wait, then close |
| `close -f [secs] [msg]` | Close and set printer free |

#### Pipe Mode Example Script

```bash
#!/bin/sh
# Example: calibration progress display

# Start fb_status in pipe mode, writing to fd 3
exec 3> >(exec /tmp/fb_status pipe -q -b)
sleep 1  # Wait for initialization

# Step 1
echo "show Preheating...\nPlease wait" >&3
# ... do preheating work ...

# Step 2
echo "color yellow" >&3
echo "show Probing bed\nStep 2 of 4" >&3
# ... do probing work ...

# Step 3
echo "color cyan" >&3
echo "show Analyzing data\nStep 3 of 4" >&3
# ... do analysis work ...

# Complete
echo "color green" >&3
echo "close -f 3 Calibration Complete!\nRestarting printer..." >&3
```

#### Response Mode vs Quiet Mode

**Response mode (default):**
```
> show Hello
OK
> color invalid
ERR: Unknown color 'invalid'
> close
OK
```

**Quiet mode (-q):**
No stdout output; errors still go to stderr.

### Process Management

Pipe mode uses PID file and file locking:
- PID file: `/tmp/fb_status.pid`
- Lock file: `/tmp/fb_status_screen.bmp.lock`

If another instance is running, new instances will fail with an error.

## Command Line Options

| Option | Long Form | Description |
|--------|-----------|-------------|
| -c | --color | Text color (name or hex RGB) |
| -g | --bg | Background color (default: 222222) |
| -s | --size | Font size in pixels (default: 32) |
| -p | --position | Position: top, center, bottom |
| -t | --timeout | Auto-hide after N seconds |
| -b | --busy | Also set printer busy (with show/pipe) |
| -f | --free | Also set printer free (with hide) |
| -B | --bold | Use bold font variant |
| -I | --italic | Use italic font variant |
| -F | --font | Custom font file path |
| -m | --message | Initial message (pipe mode only) |
| -q | --quiet | No response output (pipe mode only) |

## Colors

### Named Colors
green, red, yellow, blue, white, black, orange, cyan, magenta, gray, grey, pink, purple, lime, aqua, navy, teal, maroon, olive, silver

### Hex RGB
Supports various formats:
- `FF0000` (red)
- `#00FF00` (green)
- `0x0000FF` (blue)
- `F00` (short form, expands to FF0000)

## Screen Orientation

Auto-detected from `/userdata/app/gk/config/api.cfg`:

| Model | Model ID | Orientation |
|-------|----------|-------------|
| KS1 | 20025 | FLIP_180 |
| KS1M | 20029 | FLIP_180 |
| K3M | 20026 | ROTATE_270 |
| K3, K2P, K3V2 | 20024, 20021, 20027 | ROTATE_90 |

## Technical Details

### Display
- Resolution: 800x480
- Format: 32bpp BGRX
- Framebuffer: /dev/fb0

### Font
- Primary: /opt/rinkhals/ui/assets/AlibabaSans-Regular.ttf
- Bold: /opt/rinkhals/ui/assets/AlibabaSans-Bold.ttf
- Fallback: /oem/usr/share/simsun_en.ttf
- Default size: 32px

### Dependencies
- ffmpeg at /ac_lib/lib/third_bin/ffmpeg (for save/restore)
- LD_LIBRARY_PATH=/ac_lib/lib/third_lib (for ffmpeg)

### Binary Size
~58KB (stripped)

## Files

- `fb_status.c` - Main source code (~1250 lines)
- `stb_truetype.h` - Single-header TTF font library (from github.com/nothings/stb)
- `Makefile` - Build configuration
- `claude.md` - This documentation

## Integration with ACProxyCam

This tool is used by ACProxyCam's BedMesh calibration feature. The fb_status binary is embedded in the ACProxyCam assembly and automatically deployed to `/tmp/fb_status` on the printer when starting a calibration.

### How ACProxyCam Uses fb_status

The calibration scripts use fb_status pipe mode with heredoc to:
1. Display status messages during each calibration phase
2. Execute Native API RPC commands (Leviq2/Preheating, Leviq2/Wiping, Leviq2/Probe)
3. Show completion message and restore the screen

Example script flow:
```bash
$FB pipe -q -b << 'FB_COMMANDS'
show BedMesh Calibration\nInitializing...
rpc 30 {"id":1,"method":"Printer/ReportUIWorkStatus","params":{"busy":1}}
color yellow
show Preheating...
rpc 120 {"id":3,"method":"Leviq2/Preheating","params":{"script":"LEVIQ2_PREHEATING"}}
# ... more commands ...
color green
show Calibration Complete!
close -f 3
FB_COMMANDS
```

### RPC Command Behavior

The Native API (port 18086) sends JSON-RPC messages terminated by ETX (0x03). Key behaviors:
- API sends many status updates with `id:0` during operations
- The result for your command has your request ID (e.g., `{"id":3,"result":{}}`)
- **Connection close = command completed successfully** (API doesn't always send explicit result)
- Only explicit `"error"` in response indicates failure

### Known Issues

1. **Display flickering**: Font is reloaded on every display update. Consider caching.
2. **Large buffer truncation**: Fixed - result pattern now checked before buffer truncation.

See the ACProxyCam CLAUDE.md for the main project documentation.
